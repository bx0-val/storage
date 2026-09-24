/* Bounded parallel directory traversal. All filesystem access is read-only. */
#include <dirent.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/openat2.h>
#include <stdatomic.h>

typedef struct Entry Entry;
struct Entry {
  char *name, *rel;
  Entry *parent, *next;
  Entry **children;
  size_t count, capacity;
  uint64_t own;
  _Atomic uint64_t total;
  _Atomic unsigned pending;
  bool directory, link, excluded, failed;
};
typedef struct Inode Inode;
struct Inode { dev_t dev; ino_t ino; Inode *next; };
typedef struct {
  char *path;
  int fd;
  dev_t device;
  Entry *root, *head, *tail;
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  pthread_t threads[32];
  unsigned workers, active;
  _Atomic bool cancel, done;
  _Atomic unsigned errors;
  _Atomic uint64_t visited;
  Inode *inodes[65536];
  double started, finished;
} Scan;

static void *sx_malloc(size_t size) {
  void *p = calloc(1, size ? size : 1);
  if (!p) { perror("storage: allocation"); abort(); }
  return p;
}
static char *sx_copy(const char *s) {
  char *p = strdup(s);
  if (!p) { perror("storage: allocation"); abort(); }
  return p;
}
static double sx_now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static char *sx_join(const char *a, const char *b) {
  size_t n = strlen(a), m = strlen(b);
  if (!m) return sx_copy(a);
  char *out = sx_malloc(n + m + 2);
  memcpy(out, a, n);
  if (n && a[n-1] != '/') out[n++] = '/';
  memcpy(out+n, b, m+1);
  return out;
}
static Entry *entry_new(const char *name, char *rel, Entry *parent, bool directory) {
  Entry *e = sx_malloc(sizeof(*e));
  e->name = sx_copy(name); e->rel = rel; e->parent = parent; e->directory = directory;
  atomic_init(&e->total, 0); atomic_init(&e->pending, directory ? 1 : 0);
  return e;
}
static void entry_free(Entry *e) {
  /* Iterative postorder: deep directory trees do not consume the C stack. */
  Entry *stop = e->parent;
  while (e != stop) {
    if (e->count) { e = e->children[--e->count]; continue; }
    Entry *parent = e->parent;
    free(e->name); free(e->rel); free(e->children); free(e);
    e = parent;
  }
}
static void entry_add(Entry *parent, Entry *child) {
  if (parent->count == parent->capacity) {
    parent->capacity = parent->capacity ? parent->capacity * 2 : 32;
    void *next = realloc(parent->children, parent->capacity * sizeof(Entry *));
    if (!next) abort();
    parent->children = next;
  }
  parent->children[parent->count++] = child;
}
static void scan_enqueue(Scan *scan, Entry *entry) {
  if (scan->tail) scan->tail->next = entry;
  else scan->head = entry;
  scan->tail = entry;
  pthread_cond_signal(&scan->changed);
}
static bool scan_seen(Scan *scan, const struct stat *st) {
  if (st->st_nlink <= 1) return false;
  size_t hash = ((uint64_t)st->st_ino * UINT64_C(11400714819323198485) ^ st->st_dev) >> 48;
  pthread_mutex_lock(&scan->mutex);
  for (Inode *i = scan->inodes[hash]; i; i = i->next) {
    if (i->ino == st->st_ino && i->dev == st->st_dev) {
      pthread_mutex_unlock(&scan->mutex); return true;
    }
  }
  Inode *node = sx_malloc(sizeof(*node));
  node->ino = st->st_ino; node->dev = st->st_dev;
  node->next = scan->inodes[hash]; scan->inodes[hash] = node;
  pthread_mutex_unlock(&scan->mutex);
  return false;
}
static int scan_open(Scan *scan, const char *relative) {
  if (!*relative) return openat(scan->fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  struct open_how how = {.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC,
    .resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV};
  int fd = (int)syscall(SYS_openat2, scan->fd, relative, &how, sizeof(how));
  if (fd >= 0 || (errno != ENOSYS && errno != EINVAL)) return fd;
  /* Older-kernel fallback checks every component without following symlinks. */
  fd = dup(scan->fd);
  char *path = sx_copy(relative), *save = NULL;
  for (char *part = strtok_r(path, "/", &save); fd >= 0 && part; part = strtok_r(NULL, "/", &save)) {
    int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(fd); fd = next;
    if (fd >= 0) {
      struct stat st;
      if (fstat(fd, &st) || st.st_dev != scan->device) { close(fd); fd = -1; errno = EXDEV; }
    }
  }
  int saved = errno; free(path); errno = saved;
  return fd;
}
static void scan_directory(Scan *scan, Entry *entry) {
  uint64_t bytes = entry->own;
  int fd = scan_open(scan, entry->rel);
  DIR *dir = fd < 0 ? NULL : fdopendir(fd);
  if (!dir) {
    int saved = errno;
    if (fd >= 0) close(fd);
    pthread_mutex_lock(&scan->mutex);
    if (saved == EXDEV) { entry->excluded = true; bytes = 0; }
    else { entry->failed = true; atomic_fetch_add(&scan->errors, 1); }
    pthread_mutex_unlock(&scan->mutex);
  } else {
    for (;;) {
      if (atomic_load(&scan->cancel)) break;
      errno = 0;
      struct dirent *de = readdir(dir);
      if (!de) {
        if (errno) atomic_fetch_add(&scan->errors, 1);
        break;
      }
      if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
      struct stat st;
      if (fstatat(fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
        atomic_fetch_add(&scan->errors, 1); continue;
      }
      bool isdir = S_ISDIR(st.st_mode);
      char *relative = isdir ? sx_join(entry->rel, de->d_name) : NULL;
      Entry *child = entry_new(de->d_name, relative, entry, isdir);
      child->link = S_ISLNK(st.st_mode);
      child->own = st.st_blocks > 0 ? (uint64_t)st.st_blocks * 512 : 0;
      child->excluded = st.st_dev != scan->device;
      if (child->excluded || (!isdir && scan_seen(scan, &st))) child->own = 0;
      atomic_fetch_add(&scan->visited, 1);
      pthread_mutex_lock(&scan->mutex);
      entry_add(entry, child);
      if (isdir && !child->excluded) {
        for (Entry *p = entry; p; p = p->parent) atomic_fetch_add(&p->pending, 1);
        scan_enqueue(scan, child);
      } else {
        atomic_store(&child->pending, 0);
        atomic_store(&child->total, child->own);
        bytes += child->own;
      }
      pthread_mutex_unlock(&scan->mutex);
    }
    closedir(dir);
  }
  for (Entry *p = entry; p; p = p->parent) {
    atomic_fetch_add(&p->total, bytes);
    atomic_fetch_sub(&p->pending, 1);
  }
}
static void *scan_worker(void *argument) {
  Scan *scan = argument;
  for (;;) {
    pthread_mutex_lock(&scan->mutex);
    while (!scan->head && !atomic_load(&scan->cancel) && !atomic_load(&scan->done))
      pthread_cond_wait(&scan->changed, &scan->mutex);
    if (atomic_load(&scan->cancel) || atomic_load(&scan->done)) {
      pthread_mutex_unlock(&scan->mutex); return NULL;
    }
    Entry *entry = scan->head;
    scan->head = entry->next;
    if (!scan->head) scan->tail = NULL;
    scan->active++;
    pthread_mutex_unlock(&scan->mutex);
    scan_directory(scan, entry);
    pthread_mutex_lock(&scan->mutex);
    scan->active--;
    if (!scan->head && !scan->active) {
      scan->finished = sx_now(); atomic_store(&scan->done, true);
      pthread_cond_broadcast(&scan->changed);
    }
    pthread_mutex_unlock(&scan->mutex);
  }
}
static Scan *scan_start(const char *path, unsigned jobs) {
  int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return NULL;
  struct stat st;
  if (fstat(fd, &st)) { close(fd); return NULL; }
  Scan *scan = sx_malloc(sizeof(*scan));
  scan->fd = fd; scan->device = st.st_dev; scan->path = sx_copy(path); scan->started = sx_now();
  atomic_init(&scan->cancel, false); atomic_init(&scan->done, false);
  atomic_init(&scan->errors, 0); atomic_init(&scan->visited, 0);
  pthread_mutex_init(&scan->mutex, NULL); pthread_cond_init(&scan->changed, NULL);
  scan->root = entry_new(path, sx_copy(""), NULL, true);
  scan->root->own = (uint64_t)st.st_blocks * 512;
  scan->head = scan->tail = scan->root;
  for (unsigned i = 0; i < jobs; i++) {
    if (pthread_create(&scan->threads[scan->workers], NULL, scan_worker, scan)) break;
    scan->workers++;
  }
  if (!scan->workers) {
    atomic_store(&scan->done, true); atomic_store(&scan->errors, 1); scan->finished = sx_now();
  }
  return scan;
}
static void scan_stop(Scan *scan) {
  if (!scan) return;
  pthread_mutex_lock(&scan->mutex);
  atomic_store(&scan->cancel, true); pthread_cond_broadcast(&scan->changed);
  pthread_mutex_unlock(&scan->mutex);
  for (unsigned i = 0; i < scan->workers; i++) pthread_join(scan->threads[i], NULL);
  close(scan->fd);
  entry_free(scan->root);
  for (size_t h = 0; h < 65536; h++) {
    Inode *p = scan->inodes[h];
    while (p) { Inode *next = p->next; free(p); p = next; }
  }
  pthread_mutex_destroy(&scan->mutex); pthread_cond_destroy(&scan->changed);
  free(scan->path); free(scan);
}

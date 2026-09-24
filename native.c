/* Linux effects for the Bend application. No shells or child processes. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "scan.h"
#include "snapshot.h"

typedef struct { Entry *entry; uint64_t size; unsigned pending; } FolderRow;
typedef struct { size_t index; char *name,*right,*detail; unsigned percent; } DisplayRow;
static Snapshot storage_snapshot;
static Scan *storage_scan;
static Entry *storage_directory;
static FolderRow *folder_rows;
static size_t folder_count;
static char *storage_path;
static bool terminal_active, no_color;
static struct termios terminal_before;
static volatile sig_atomic_t interrupted_signal;
static unsigned storage_jobs=4;
static double storage_interval=5;
static int storage_exit;
static char storage_note[256];
static char *last_screen;
static size_t last_screen_size;

static void write_all(int fd,const char *text,size_t n) {
  while(n) {
    ssize_t wrote=write(fd,text,n);
    if(wrote<0) {if(errno==EINTR)continue;break;}
    if(!wrote)break;
    text+=wrote;n-=(size_t)wrote;
  }
}
static void terminal_restore(void) {
  if(!terminal_active)return;
  tcsetattr(STDIN_FILENO,TCSANOW,&terminal_before);
  const char *restore="\033[0m\033[?25h\033[?1049l";
  write_all(STDOUT_FILENO,restore,strlen(restore));
  terminal_active=false;
}
static void storage_signal(int number) { interrupted_signal=number; }
static void storage_cleanup(void) {
  terminal_restore();
  scan_stop(storage_scan);storage_scan=NULL;storage_directory=NULL;
  snapshot_free(&storage_snapshot);
  free(folder_rows);folder_rows=NULL;folder_count=0;
  free(storage_path);storage_path=NULL;
  free(last_screen);last_screen=NULL;last_screen_size=0;
}
static void storage_fail(const char *message) {
  terminal_restore();fprintf(stderr,"storage: %s\n",message);exit(2);
}
static bool fs_visible(Filesystem *fs,bool all) {return all||!virtual_type(fs->type)||!strcmp(fs->mount,"/");}
static size_t fs_count(bool all) {
  size_t n=0;for(size_t i=0;i<storage_snapshot.count;i++)if(fs_visible(&storage_snapshot.fs[i],all))n++;
  return n;
}
static Filesystem *fs_at(size_t index,bool all) {
  for(size_t i=0;i<storage_snapshot.count;i++)if(fs_visible(&storage_snapshot.fs[i],all)) {
    if(!index--)return &storage_snapshot.fs[i];
  }
  return NULL;
}
static char *entry_path(Entry *entry) {
  if(entry->directory)return sx_join(storage_scan->path,entry->rel);
  char *parent=sx_join(storage_scan->path,entry->parent->rel),*result=sx_join(parent,entry->name);
  free(parent);return result;
}
static void set_directory(Entry *entry) {
  storage_directory=entry;free(storage_path);storage_path=entry_path(entry);
  free(folder_rows);folder_rows=NULL;folder_count=0;
}
static bool open_directory(const char *path,bool force) {
  char *canonical=realpath(path,NULL);
  if(!canonical) {snprintf(storage_note,sizeof(storage_note),"%s",strerror(errno));return false;}
  struct stat st;
  if(stat(canonical,&st)||!S_ISDIR(st.st_mode)) {snprintf(storage_note,sizeof(storage_note),"Path is not an accessible directory");free(canonical);return false;}
  if(!force&&storage_scan) {
    size_t n=strlen(storage_scan->path);
    bool inside=!strcmp(canonical,storage_scan->path)||
      (!strncmp(canonical,storage_scan->path,n)&&(canonical[n]=='/'||!strcmp(storage_scan->path,"/")));
    if(inside) {
      Entry *found=storage_scan->root;
      const char *relative=canonical+n;while(*relative=='/')relative++;
      char *copy=sx_copy(relative),*save=NULL;
      pthread_mutex_lock(&storage_scan->mutex);
      for(char *part=strtok_r(copy,"/",&save);found&&part;part=strtok_r(NULL,"/",&save)) {
        Entry *next=NULL;
        for(size_t i=0;i<found->count;i++)if(found->children[i]->directory&&!strcmp(found->children[i]->name,part)){next=found->children[i];break;}
        found=next;
      }
      bool usable=found&&!found->excluded&&!found->failed;
      pthread_mutex_unlock(&storage_scan->mutex);free(copy);
      if(usable) {set_directory(found);free(canonical);return true;}
    }
  }
  Scan *next=scan_start(canonical,storage_jobs);
  if(!next) {snprintf(storage_note,sizeof(storage_note),"%s",strerror(errno));free(canonical);return false;}
  scan_stop(storage_scan);storage_scan=next;set_directory(next->root);free(canonical);
  return true;
}
static int folder_compare(const void *a,const void *b) {
  const FolderRow *x=a,*y=b;
  if(x->size!=y->size)return x->size>y->size?-1:1;
  return strcmp(x->entry->name,y->entry->name);
}
static void folder_update(void) {
  free(folder_rows);folder_rows=NULL;folder_count=0;
  if(!storage_scan||!storage_directory)return;
  pthread_mutex_lock(&storage_scan->mutex);
  folder_count=storage_directory->count;
  folder_rows=sx_malloc(folder_count*sizeof(*folder_rows));
  for(size_t i=0;i<folder_count;i++) {
    Entry *e=storage_directory->children[i];
    folder_rows[i]=(FolderRow){e,atomic_load(&e->total),atomic_load(&e->pending)};
  }
  pthread_mutex_unlock(&storage_scan->mutex);
  qsort(folder_rows,folder_count,sizeof(*folder_rows),folder_compare);
}
static void json_device(Device *d,bool children) {
  printf("{\"name\":");json_string(stdout,d->name);
  printf(",\"type\":");json_string(stdout,d->type);
  printf(",\"size\":%"PRIu64",\"model\":",d->size);json_string(stdout,d->model);
  printf(",\"fstype\":");json_string(stdout,d->fstype);
  printf(",\"mountpoints\":[");if(*d->mount)json_string(stdout,d->mount);else printf("null");printf("]");
  if(children) {
    printf(",\"children\":[");bool comma=false;
    for(size_t i=0;i<storage_snapshot.device_count;i++)if(!strcmp(storage_snapshot.devices[i].parent,d->name)) {
      if(comma)printf(",");comma=true;json_device(&storage_snapshot.devices[i],false);
    }
    printf("]");
  }
  printf("}");
}
static void output_json(bool all,bool directory) {
  Snapshot *s=&storage_snapshot;
  printf("{\"host\":");json_string(stdout,s->host);printf(",\"updated\":");json_string(stdout,s->updated);
  printf(",\"filesystems\":[");bool comma=false;
  for(size_t i=0;i<s->count;i++) {
    Filesystem *f=&s->fs[i];if(!fs_visible(f,all))continue;
    if(comma)printf(",");comma=true;
    printf("{\"mount\":");json_string(stdout,f->mount);printf(",\"source\":");json_string(stdout,f->source);
    printf(",\"type\":");json_string(stdout,f->type);
    printf(",\"total\":%"PRIu64",\"used\":%"PRIu64",\"available\":%"PRIu64",\"reserved\":%"PRIu64",\"percent\":%.9g,\"inodes\":%"PRIu64",\"inodes_used\":%"PRIu64",\"inode_percent\":",f->total,f->used,f->available,f->reserved,f->percent,f->inodes,f->inodes_used);
    if(f->inodes)printf("%.9g",100.0*(double)f->inodes_used/(double)f->inodes);else printf("null");printf("}");
  }
  printf("],\"devices\":[");comma=false;
  for(size_t i=0;i<s->device_count;i++)if(!*s->devices[i].parent) {if(comma)printf(",");comma=true;json_device(&s->devices[i],true);}
  printf("],\"swap\":[");
  for(size_t i=0;i<s->swap_count;i++) {
    Swap *swap=&s->swap[i];if(i)printf(",");printf("{\"name\":");json_string(stdout,swap->name);
    printf(",\"type\":");json_string(stdout,swap->type);
    printf(",\"total\":%"PRIu64",\"used\":%"PRIu64",\"priority\":%d}",swap->total,swap->used,swap->priority);
  }
  printf("],\"warnings\":[");if(s->errors)printf("\"Some local storage information could not be read\"");printf("]");
  if(directory&&storage_scan) {
    folder_update();printf(",\"directory\":{\"path\":");json_string(stdout,storage_path);
    printf(",\"total\":%"PRIu64",\"complete\":%s,\"scanned_entries\":%"PRIu64",\"scan_seconds\":%.6f,\"entries\":[",atomic_load(&storage_directory->total),atomic_load(&storage_scan->done)?"true":"false",atomic_load(&storage_scan->visited),storage_scan->finished-storage_scan->started);
    for(size_t i=0;i<folder_count;i++) {
      Entry *e=folder_rows[i].entry;if(i)printf(",");printf("{\"path\":");char *path=entry_path(e);json_string(stdout,path);free(path);
      printf(",\"name\":");json_string(stdout,e->name);
      printf(",\"size\":%"PRIu64",\"directory\":%s}",folder_rows[i].size,e->directory?"true":"false");
    }
    printf("],\"warning\":");json_string(stdout,atomic_load(&storage_scan->errors)?"Partial scan: some entries could not be read":"");printf("}");
  }
  printf("}\n");
}
static void output_plain(bool all,bool directory) {
  Snapshot *s=&storage_snapshot;char a[32],b[32],c[32];
  printf("STORAGE  %s  %s\n\nFILESYSTEMS\n",s->host,s->updated);
  for(size_t i=0;i<s->count;i++) {
    Filesystem *f=&s->fs[i];if(!fs_visible(f,all))continue;
    char *mount=safe_text(f->mount,200),*source=safe_text(f->source,100);
    printf("  %s  %s  %s\n",mount,source,f->type);free(mount);free(source);
    unsigned filled=(unsigned)(f->percent/5);if(filled>20)filled=20;
    printf("  [");for(unsigned j=0;j<20;j++)putchar(j<filled?'#':'-');
    printf("] %5.1f%%  %s used / %s total  %s available\n",f->percent,human_bytes(f->used,a),human_bytes(f->total,b),human_bytes(f->available,c));
    if(f->inodes)printf("  Inodes %.1f%%",100.0*(double)f->inodes_used/(double)f->inodes);else printf("  Inodes n/a");
    printf("  Reserved %s\n",human_bytes(f->reserved,a));
  }
  printf("\nDISKS & PARTITIONS\n");
  for(size_t i=0;i<s->device_count;i++) {
    Device *d=&s->devices[i];char *mount=safe_text(d->mount,200),*model=safe_text(d->model,100);
    printf("  %-12s %10s  %-5s  %s  %s\n",d->name,human_bytes(d->size,a),d->type,model,*mount?mount:"unmounted");free(mount);free(model);
  }
  printf("\nSWAP\n");
  if(!s->swap_count)printf("  No swap configured.\n");
  for(size_t i=0;i<s->swap_count;i++)printf("  %s used / %s total\n",human_bytes(s->swap[i].used,a),human_bytes(s->swap[i].total,b));
  if(directory&&storage_scan) {
    folder_update();char *path=safe_text(storage_path,300);
    printf("\nFOLDER  %s  (%s allocated)\n",path,human_bytes(atomic_load(&storage_directory->total),a));free(path);
    for(size_t i=0;i<folder_count;i++) {
      Entry *e=folder_rows[i].entry;char *name=safe_text(e->name,300);
      printf("  %10s  %s%s\n",human_bytes(folder_rows[i].size,a),name,e->directory?"/":"");free(name);
    }
    if(atomic_load(&storage_scan->errors))printf("  Partial scan: some entries could not be read.\n");
  }
  if(s->errors)printf("\nNote: some local storage information could not be read.\n");
}
static Term native_record(Env e,u64 cid,size_t count,Term *fields) {
  if(cid_arity(cid)!=count)storage_fail("Bend effect layout mismatch; rebuild with Bend 2.0.27");
  Loc loc=heap_alloc(e,cls_fit(count));
  for(size_t i=0;i<count;i++)e.mem[loc+i]=io_seal(e,fields[i],cid);
  return term_ctr(cid,loc);
}
static Term native_text(Env e,const char *text,int width) {
  char *safe=safe_text(text,width);Term result=io_str(e,safe,strlen(safe));free(safe);return result;
}
static void native_unpack(Env e,Term value,size_t n,Term *fields) {
  spare_free(e,cls_fit(n),ctr_take(e,value,n,fields));
}
static void terminal_size(unsigned *width,unsigned *height) {
  struct winsize size={0};ioctl(STDOUT_FILENO,TIOCGWINSZ,&size);
  *width=size.ws_col?size.ws_col:80;*height=size.ws_row?size.ws_row:24;
}
static size_t index_bound(size_t index,size_t count) {return count?(index<count?index:count-1):0;}

Term native_init_run(Env e,Term *f,IoWork *w) {
  (void)f;(void)w;
  setlocale(LC_CTYPE,"");if(MB_CUR_MAX<4)setlocale(LC_CTYPE,"C.UTF-8");
  bool all=false,json=false,plain=false;const char *path=NULL;
  for(int i=0;i<io_argc;i++) {
    const char *a=io_argv[i];
    if(!strcmp(a,"--help")||!strcmp(a,"-h")) {
      puts("Usage: storage [--plain|--json] [--all] [--jobs N] [--interval SECONDS] [DIRECTORY]\n\nFast Linux storage dashboard, compiled from Bend.\n  --plain, --once  Print one snapshot (automatic when piped)\n  --json          Machine-readable snapshot; DIRECTORY adds a full scan\n  --all, -a       Include RAM and other virtual filesystems\n  --jobs N        Parallel scan workers, 1..32 (default: 4)\n  --interval SEC  Filesystem refresh interval, at least 1 (default: 5)\n\nKeys: Tab or 1/2/3 views; arrows or j/k select; Enter browse; h parent;\nH home; r refresh; a virtual mounts; ? help; q quit.\nFolder results appear during scanning. Browsing reuses the scanned tree.");
      return term_pak(CID_APP_STOP,0);
    } else if(!strcmp(a,"--version")) {puts("storage 2.0 (Bend 2.0.27; native Linux effects)");return term_pak(CID_APP_STOP,0);}
    else if(!strcmp(a,"--all")||!strcmp(a,"-a"))all=true;
    else if(!strcmp(a,"--json"))json=true;
    else if(!strcmp(a,"--plain")||!strcmp(a,"--once"))plain=true;
    else if(!strcmp(a,"--jobs")||!strcmp(a,"--interval")) {
      bool jobs=!strcmp(a,"--jobs");if(++i==io_argc)storage_fail("missing option value");
      char *end=NULL;double value=strtod(io_argv[i],&end);
      if(!*io_argv[i]||*end||!isfinite(value)||value<1||(jobs&&(value>32||floor(value)!=value)))storage_fail("invalid jobs or interval value");
      if(jobs)storage_jobs=(unsigned)value;else storage_interval=value;
    } else if(!strcmp(a,"--")) {
      if(++i<io_argc) {if(path)storage_fail("only one directory is allowed");path=io_argv[i];}
      if(i+1<io_argc)storage_fail("only one directory is allowed");break;
    } else if(a[0]=='-')storage_fail("unknown option; use storage --help");
    else {if(path)storage_fail("only one directory is allowed");path=a;}
  }
  if(json&&plain)storage_fail("choose either --json or --plain");
  no_color=getenv("NO_COLOR")!=NULL;
  atexit(storage_cleanup);
  struct sigaction sa={0};sa.sa_handler=storage_signal;sigemptyset(&sa.sa_mask);
  sigaction(SIGINT,&sa,NULL);sigaction(SIGTERM,&sa,NULL);sigaction(SIGHUP,&sa,NULL);
  signal(SIGPIPE,SIG_IGN);
  if(path) {
    if(!open_directory(path,false))storage_fail(storage_note);
  } else {
    const char *home=getenv("HOME");storage_path=sx_copy(home?home:".");
  }
  snapshot_collect(&storage_snapshot);
  const char *term=getenv("TERM");
  if(json||plain||!isatty(0)||!isatty(1)||!term||!strcmp(term,"dumb")) {
    if(storage_scan)while(!atomic_load(&storage_scan->done)&&!interrupted_signal) {
      struct timespec delay={0,10000000};nanosleep(&delay,NULL);
    }
    if(!interrupted_signal) {if(json)output_json(all,path!=NULL);else output_plain(all,path!=NULL);}
    storage_exit=storage_snapshot.count?0:1;
    return term_pak(CID_APP_STOP,0);
  }
  if(tcgetattr(0,&terminal_before))storage_fail("cannot read terminal settings");
  struct termios raw=terminal_before;cfmakeraw(&raw);raw.c_lflag|=ISIG;
  if(tcsetattr(0,TCSANOW,&raw))storage_fail("cannot set terminal mode");
  terminal_active=true;
  const char *start="\033[?1049h\033[?25l";write_all(1,start,strlen(start));
  /* Bend 2.0.27 unboxes enum and Bool fields inside product records. */
  /* Continue flattens State (four scalar fields) followed by Action. */
  Term fields[]={path?2:0,0,all?1:0,0,0};
  return native_record(e,CID_APP_CONTINUE,5,fields);
}
Term native_action_run(Env e,Term *f,IoWork *w) {
  (void)w;Term fields[4];native_unpack(e,f[1],4,fields);
  unsigned tab=(unsigned)fields[0],action=term_aux(f[0]);size_t selected=(size_t)fields[1];
  bool all=fields[2]!=0;
  storage_note[0]=0;
  if(action==CID_APP_RESTAT)snapshot_collect(&storage_snapshot);
  if(action==CID_APP_HOMEDIR) {
    const char *home=getenv("HOME");if(open_directory(home?home:".",false)){fields[0]=2;fields[1]=0;}
  } else if(action==CID_APP_RESCAN&&tab==2) {
    if(open_directory(storage_path,true))fields[1]=0;
  } else if(action==CID_APP_PARENT&&tab==2) {
    if(storage_directory&&storage_directory->parent) {set_directory(storage_directory->parent);fields[1]=0;}
    else {char *parent=sx_copy(storage_path);char *last=strrchr(parent,'/');if(last) {if(last==parent)last[1]=0;else *last=0;}if(open_directory(parent,false))fields[1]=0;free(parent);}
  } else if(action==CID_APP_OPEN) {
    char *target=NULL;
    if(tab==0) {Filesystem *fs=fs_at(selected,all);if(fs)target=sx_copy(fs->mount);}
    else if(tab==1&&selected<storage_snapshot.device_count&&*storage_snapshot.devices[selected].mount)target=sx_copy(storage_snapshot.devices[selected].mount);
    else if(tab==2&&selected<folder_count&&folder_rows[selected].entry->directory) {
      Entry *entry=folder_rows[selected].entry;
      pthread_mutex_lock(&storage_scan->mutex);bool excluded=entry->excluded,failed=entry->failed;pthread_mutex_unlock(&storage_scan->mutex);
      if(!excluded&&!failed) {set_directory(entry);fields[1]=0;}
      else target=entry_path(entry);
    }
    if(target) {
      if(open_directory(target,false)){fields[0]=2;fields[1]=0;}
      free(target);
    }
  }
  return native_record(e,CID_APP_STATE,4,fields);
}
Term native_frame_run(Env e,Term *f,IoWork *w) {
  (void)w;Term state[4];native_unpack(e,f[0],4,state);
  unsigned tab=(unsigned)state[0],width,height;terminal_size(&width,&height);
  bool all=state[2]!=0;
  if(sx_now()-storage_snapshot.collected>=storage_interval)snapshot_collect(&storage_snapshot);
  if(tab==2&&!storage_scan)open_directory(storage_path,false);
  if(tab==2)folder_update();
  size_t count=tab==0?fs_count(all):tab==1?storage_snapshot.device_count:folder_count;
  size_t selected=index_bound((size_t)state[1],count);
  size_t visible=height>14?(height-14)/3:1;if(!visible)visible=1;
  size_t start=selected>=visible?selected-visible+1:0;
  char title[1024],summary[1024],status[512],a[32],b[32],c[32];
  status[0]=0;
  if(tab==0) {
    snprintf(title,sizeof(title),"MOUNTED FILESYSTEMS   |   %s",storage_snapshot.host);
    Filesystem *fs=fs_at(selected,all);
    if(fs) {
      snprintf(summary,sizeof(summary),"%s used   /   %s capacity   /   %s available",human_bytes(fs->used,a),human_bytes(fs->total,b),human_bytes(fs->available,c));
      if(fs->inodes)snprintf(status,sizeof(status),"Inodes %.1f%%  |  Reserved %s",100.0*(double)fs->inodes_used/(double)fs->inodes,human_bytes(fs->reserved,a));
      else snprintf(status,sizeof(status),"Inodes n/a  |  Reserved %s",human_bytes(fs->reserved,a));
    } else snprintf(summary,sizeof(summary),"No local filesystems could be read");
  } else if(tab==1) {
    snprintf(title,sizeof(title),"DISKS & PARTITIONS");
    uint64_t used=0,total=0;for(size_t i=0;i<storage_snapshot.swap_count;i++){used+=storage_snapshot.swap[i].used;total+=storage_snapshot.swap[i].total;}
    if(total)snprintf(summary,sizeof(summary),"Swap: %s used / %s total",human_bytes(used,a),human_bytes(total,b));
    else snprintf(summary,sizeof(summary),"No swap configured");
    snprintf(status,sizeof(status),"Enter browses a mounted partition");
  } else {
    snprintf(title,sizeof(title),"%s",storage_path);
    uint64_t measured=storage_directory?atomic_load(&storage_directory->total):0;
    bool done=storage_directory&&!atomic_load(&storage_directory->pending);
    if(done)snprintf(summary,sizeof(summary),"%s allocated  |  Cached: navigation is instant",human_bytes(measured,a));
    else snprintf(summary,sizeof(summary),"Scanning: %s measured  |  %"PRIu64" entries visited  |  %u workers",human_bytes(measured,a),storage_scan?atomic_load(&storage_scan->visited):0,storage_jobs);
    snprintf(status,sizeof(status),"%s",storage_scan&&atomic_load(&storage_scan->errors)?"Partial scan: unreadable entries":"Allocated space; symlinks are not followed");
  }
  if(*storage_note)snprintf(status,sizeof(status),"%s",storage_note);
  Term rows=term_pak(CID_NIL,0);
  size_t end=start+visible;if(end>count)end=count;
  for(size_t i=end;i>start;) {
    --i;char name[1024],right[128],detail[1024];unsigned percentage=0;
    if(tab==0) {
      Filesystem *fs=fs_at(i,all);snprintf(name,sizeof(name),"%s",fs->mount);
      snprintf(right,sizeof(right),"%s / %s",human_bytes(fs->used,a),human_bytes(fs->total,b));
      snprintf(detail,sizeof(detail),"%5.1f%%  %s  %s",fs->percent,fs->type,fs->source);percentage=(unsigned)fs->percent;
    } else if(tab==1) {
      Device *d=&storage_snapshot.devices[i];snprintf(name,sizeof(name),"%s%s",*d->parent?"  └─ ":"",d->name);
      snprintf(right,sizeof(right),"%s",human_bytes(d->size,a));
      snprintf(detail,sizeof(detail),"%s %s %s",d->type,*d->mount?d->mount:"unmounted",d->model);
    } else {
      FolderRow row=folder_rows[i];Entry *entry=row.entry;
      snprintf(name,sizeof(name),"%s%s",entry->name,entry->directory?"/":"");
      snprintf(right,sizeof(right),"%s%s",human_bytes(row.size,a),row.pending?" +":"");
      pthread_mutex_lock(&storage_scan->mutex);bool excluded=entry->excluded;pthread_mutex_unlock(&storage_scan->mutex);
      snprintf(detail,sizeof(detail),"%s",excluded?"Other filesystem; Enter to scan":row.pending?"Scanning…":entry->directory?"Directory · Enter to open":entry->link?"Symlink (target not scanned)":"File");
      uint64_t total=atomic_load(&storage_directory->total);percentage=total?(unsigned)(100.0*(double)row.size/(double)total):0;
    }
    Term fields[]={(Term)i,native_text(e,name,(int)width-36),native_text(e,right,28),native_text(e,detail,(int)width-30),(Term)percentage};
    rows=io_node(e,CID_CON,native_record(e,CID_APP_ROW,5,fields),rows);
  }
  Term frame[]={(Term)width,(Term)height,(Term)count,native_text(e,title,(int)width-5),native_text(e,summary,(int)width-5),native_text(e,status,(int)width-22),rows};
  return native_record(e,CID_APP_FRAME,7,frame);
}
Term native_write_run(Env e,Term *f,IoWork *w) {
  (void)w;u64 n;char *text=io_cstr(e,f[0],&n);
  if(no_color) {
    char *in=text,*out=text;
    while(*in) {
      if(in[0]==27&&in[1]=='[') {
        char *end=in+2;while(isdigit((unsigned char)*end)||*end==';')end++;
        if(*end=='m'){in=end+1;continue;}
      }
      *out++=*in++;
    }
    n=(u64)(out-text);
  }
  /* Avoid repainting an unchanged screen on every idle tick, especially over SSH. */
  if(!last_screen||n!=last_screen_size||memcmp(text,last_screen,(size_t)n)) {
    write_all(1,text,(size_t)n);
    free(last_screen);last_screen=text;last_screen_size=(size_t)n;
  } else free(text);
  return term_pak(CID_UNIT,0);
}
static int read_key_byte(int timeout) {
  struct pollfd poller={.fd=0,.events=POLLIN};
  int ready=poll(&poller,1,timeout);
  if(ready<=0)return -1;
  unsigned char c;ssize_t n=read(0,&c,1);return n==1?c:-2;
}
Term native_key_run(Env e,Term *f,IoWork *w) {
  (void)e;(void)f;(void)w;
  if(interrupted_signal)return term_pak(CID_APP_QUIT,0);
  int c=read_key_byte(100);unsigned key=CID_APP_TICK;
  if(c==-2||interrupted_signal)key=CID_APP_QUIT;
  else if(c==27) {
    int next=read_key_byte(25);
    if(next=='['||next=='O') {
      int last=read_key_byte(25);
      if(last=='A')key=CID_APP_UP;else if(last=='B')key=CID_APP_DOWN;
      else if(last=='C')key=CID_APP_ENTER;else if(last=='D')key=CID_APP_BACK;
      else if(last=='H')key=CID_APP_FIRST;else if(last=='F')key=CID_APP_LAST;
      else if(last>='1'&&last<='6') {
        int end=read_key_byte(25);
        if(end=='~')key=last=='5'?CID_APP_PAGEUP:last=='6'?CID_APP_PAGEDOWN:last=='1'?CID_APP_FIRST:last=='4'?CID_APP_LAST:CID_APP_TICK;
      }
    } else key=CID_APP_QUIT;
  } else switch(c) {
    case 'q':case 3:key=CID_APP_QUIT;break;
    case '\t':key=CID_APP_CYCLE;break;
    case '1':key=CID_APP_SHOWOVERVIEW;break;case '2':key=CID_APP_SHOWDISKS;break;case '3':key=CID_APP_SHOWFOLDERS;break;
    case 'k':key=CID_APP_UP;break;case 'j':key=CID_APP_DOWN;break;
    case '\r':case '\n':case 'l':key=CID_APP_ENTER;break;
    case 8:case 127:case 'h':key=CID_APP_BACK;break;
    case 'H':key=CID_APP_HOME;break;case 'r':key=CID_APP_REFRESH;break;
    case 'a':key=CID_APP_TOGGLEALL;break;case '?':key=CID_APP_TOGGLEHELP;break;
  }
  return term_pak(key,0);
}
Term native_close_run(Env e,Term *f,IoWork *w) {
  (void)e;(void)f;(void)w;storage_cleanup();
  if(interrupted_signal)exit(128+interrupted_signal);
  if(storage_exit)exit(storage_exit);
  return term_pak(CID_UNIT,0);
}
static void __attribute__((constructor)) native_register(void) {
  io_eff(CID_NATIVE_INIT,native_init_run,0);io_eff(CID_NATIVE_ACTION,native_action_run,0);
  io_eff(CID_NATIVE_FRAME,native_frame_run,0);io_eff(CID_NATIVE_WRITE,native_write_run,0);
  io_eff(CID_NATIVE_KEY,native_key_run,0);io_eff(CID_NATIVE_CLOSE,native_close_run,0);
}

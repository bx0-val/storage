#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <wchar.h>
#include <wctype.h>
#include <locale.h>

typedef struct {
  char *mount, *source, *type;
  unsigned major, minor;
  uint64_t total, used, available, reserved, inodes, inodes_used;
  double percent;
} Filesystem;
typedef struct {
  char *name, *parent, *model, *type, *mount, *fstype;
  uint64_t size;
} Device;
typedef struct { char *name, *type; uint64_t total, used; int priority; } Swap;
typedef struct {
  Filesystem *fs; size_t count;
  Device *devices; size_t device_count;
  Swap *swap; size_t swap_count;
  char host[256], updated[64];
  unsigned errors;
  double collected;
} Snapshot;

static char *unescape_mount(const char *s) {
  char *out = sx_malloc(strlen(s)+1), *p = out;
  while (*s) {
    if (s[0]=='\\' && s[1]>='0' && s[1]<='7' && s[2]>='0' && s[2]<='7' && s[3]>='0' && s[3]<='7') {
      *p++ = (char)((s[1]-'0')*64+(s[2]-'0')*8+s[3]-'0'); s += 4;
    } else *p++ = *s++;
  }
  return out;
}
static bool virtual_type(const char *type) {
  return !strcmp(type,"tmpfs") || !strcmp(type,"devtmpfs") || !strcmp(type,"squashfs") || !strcmp(type,"overlay") || !strcmp(type,"ramfs");
}
static char *read_text(const char *path) {
  FILE *f = fopen(path,"re");
  if (!f) return sx_copy("");
  char *line = NULL; size_t cap=0;
  if (getline(&line,&cap,f)<0) { free(line); line=sx_copy(""); }
  fclose(f);
  size_t n=strlen(line); while(n && isspace((unsigned char)line[n-1])) line[--n]=0;
  return line;
}
static void snapshot_free(Snapshot *s) {
  for(size_t i=0;i<s->count;i++) { free(s->fs[i].mount); free(s->fs[i].source); free(s->fs[i].type); }
  for(size_t i=0;i<s->device_count;i++) {
    Device *d=&s->devices[i]; free(d->name);free(d->parent);free(d->model);free(d->type);free(d->mount);free(d->fstype);
  }
  for(size_t i=0;i<s->swap_count;i++) { free(s->swap[i].name); free(s->swap[i].type); }
  free(s->fs); free(s->devices); free(s->swap); memset(s,0,sizeof(*s));
}
static int fs_compare(const void *a,const void *b) {
  const Filesystem *x=a,*y=b;
  if(!strcmp(x->mount,"/")) return strcmp(y->mount,"/") ? -1 : 0;
  if(!strcmp(y->mount,"/")) return 1;
  return strcmp(x->mount,y->mount);
}
static int device_compare(const void *a,const void *b) {
  return strcmp(((const Device*)a)->name,((const Device*)b)->name);
}
static void snapshot_collect(Snapshot *s) {
  snapshot_free(s);
  gethostname(s->host,sizeof(s->host)-1);
  time_t now=time(NULL); struct tm local; localtime_r(&now,&local);
  strftime(s->updated,sizeof(s->updated),"%Y-%m-%dT%H:%M:%S%z",&local);
  FILE *f=fopen("/proc/self/mountinfo","re");
  if(f) {
    char *line=NULL;size_t cap=0;
    while(getline(&line,&cap,f)>=0) {
      char *sep=strstr(line," - "); if(!sep)continue; *sep=0;
      char *tail=sep+3,*save=NULL;
      char *type=strtok_r(tail," \n",&save),*source=strtok_r(NULL," \n",&save);
      char *fields[6],*walk=NULL;unsigned n=0;
      for(char *p=strtok_r(line," ",&walk);p && n<6;p=strtok_r(NULL," ",&walk))fields[n++]=p;
      if(n<6 || !type || !source)continue;
      char *mount=unescape_mount(fields[4]);
      /* Local devices plus RAM-backed mounts; never wait for network storage. */
      if(strcmp(mount,"/") && strncmp(source,"/dev/",5) && !virtual_type(type)) {free(mount);continue;}
      if(!strcmp(type,"overlay") && strcmp(mount,"/")) {free(mount);continue;}
      bool duplicate=false;
      for(size_t i=0;i<s->count;i++)if(!strcmp(s->fs[i].mount,mount))duplicate=true;
      if(duplicate) {free(mount);continue;}
      struct statvfs st;
      if(statvfs(mount,&st) || !st.f_blocks) {s->errors++;free(mount);continue;}
      s->fs=realloc(s->fs,(s->count+1)*sizeof(*s->fs));if(!s->fs)abort();
      Filesystem *fs=&s->fs[s->count++];memset(fs,0,sizeof(*fs));
      fs->mount=mount;fs->source=unescape_mount(source);fs->type=sx_copy(type);
      sscanf(fields[2],"%u:%u",&fs->major,&fs->minor);
      fs->total=(uint64_t)st.f_blocks*st.f_frsize;
      fs->used=(uint64_t)(st.f_blocks-st.f_bfree)*st.f_frsize;
      fs->available=(uint64_t)st.f_bavail*st.f_frsize;
      fs->reserved=fs->total>=fs->used+fs->available?fs->total-fs->used-fs->available:0;
      fs->inodes=st.f_files;fs->inodes_used=st.f_files-st.f_ffree;
      fs->percent=fs->used+fs->available?100.0*(double)fs->used/(double)(fs->used+fs->available):0;
    }
    free(line);fclose(f);
    qsort(s->fs,s->count,sizeof(*s->fs),fs_compare);
  } else s->errors++;
  DIR *dir=opendir("/sys/class/block");
  if(dir) {
    struct dirent *de;
    while((de=readdir(dir))) {
      if(de->d_name[0]=='.')continue;
      char base[512],path[640];snprintf(base,sizeof(base),"/sys/class/block/%s",de->d_name);
      snprintf(path,sizeof(path),"%s/size",base);char *size=read_text(path);
      uint64_t bytes=strtoull(size,NULL,10)*512;free(size);if(!bytes)continue;
      s->devices=realloc(s->devices,(s->device_count+1)*sizeof(*s->devices));if(!s->devices)abort();
      Device *d=&s->devices[s->device_count++];memset(d,0,sizeof(*d));
      d->name=sx_copy(de->d_name);d->size=bytes;d->parent=sx_copy("");
      snprintf(path,sizeof(path),"%s/partition",base);
      bool part=access(path,F_OK)==0;
      d->type=sx_copy(part?"part":!strncmp(d->name,"sr",2)?"rom":!strncmp(d->name,"loop",4)?"loop":!strncmp(d->name,"dm-",3)?"dm":"disk");
      if(part) {
        char link[1024];ssize_t len=readlink(base,link,sizeof(link)-1);
        if(len>0) {link[len]=0;char *slash=strrchr(link,'/');if(slash)*slash=0;slash=strrchr(link,'/');free(d->parent);d->parent=sx_copy(slash?slash+1:link);}
      }
      snprintf(path,sizeof(path),"%s/device/model",base);d->model=read_text(path);
      snprintf(path,sizeof(path),"%s/dev",base);char *dev=read_text(path);unsigned major=0,minor=0;sscanf(dev,"%u:%u",&major,&minor);free(dev);
      d->mount=sx_copy("");d->fstype=sx_copy("");
      for(size_t i=0;i<s->count;i++)if(s->fs[i].major==major&&s->fs[i].minor==minor) {
        free(d->mount);free(d->fstype);d->mount=sx_copy(s->fs[i].mount);d->fstype=sx_copy(s->fs[i].type);break;
      }
    }
    closedir(dir);qsort(s->devices,s->device_count,sizeof(*s->devices),device_compare);
  } else s->errors++;
  f=fopen("/proc/swaps","re");
  if(f) {
    char *line=NULL;size_t cap=0;getline(&line,&cap,f);
    while(getline(&line,&cap,f)>=0) {
      char name[4096],type[32];unsigned long long total,used;int priority;
      if(sscanf(line,"%4095s %31s %llu %llu %d",name,type,&total,&used,&priority)!=5)continue;
      s->swap=realloc(s->swap,(s->swap_count+1)*sizeof(*s->swap));if(!s->swap)abort();
      s->swap[s->swap_count++]=(Swap){unescape_mount(name),sx_copy(type),total*1024,used*1024,priority};
    }
    free(line);fclose(f);
  } else s->errors++;
  s->collected=sx_now();
}
static char *human_bytes(uint64_t bytes,char out[32]) {
  static const char *units[]={"B","KiB","MiB","GiB","TiB","PiB","EiB"};
  unsigned unit=0;double value=(double)bytes;
  while(value>=1024 && unit<6) {value/=1024;unit++;}
  if(unit)snprintf(out,32,"%.1f %s",value,units[unit]);else snprintf(out,32,"%llu B",(unsigned long long)bytes);
  return out;
}
static void json_string(FILE *f,const char *s) {
  fputc('"',f);mbstate_t state={0};
  while(*s) {
    unsigned char c=*s;
    if(c=='"'||c=='\\') {fputc('\\',f);fputc(c,f);s++;}
    else if(c<32) {fprintf(f,"\\u%04x",c);s++;}
    else if(c<128) {fputc(c,f);s++;}
    else {
      wchar_t wc;size_t n=mbrtowc(&wc,s,strlen(s),&state);
      if(n==(size_t)-1||n==(size_t)-2) {fprintf(f,"\\udc%02x",c);s++;memset(&state,0,sizeof(state));}
      else {fwrite(s,1,n,f);s+=n;}
    }
  }
  fputc('"',f);
}
static char *safe_text(const char *input,int width) {
  size_t length=strlen(input);char *out=sx_malloc(length*3+1),*target=out;
  mbstate_t state={0};int cells=0;
  while(*input && cells<width) {
    wchar_t wc;size_t n=mbrtowc(&wc,input,strlen(input),&state);
    if(n==(size_t)-1||n==(size_t)-2||!n) {n=1;wc=L'?';memset(&state,0,sizeof(state));}
    int size=wcwidth(wc);
    if(size<0||wc==27) {wc=L'?';size=1;}
    if(cells+size>width)break;
    if(wc==L'?')*target++='?';else {memcpy(target,input,n);target+=n;}
    input+=n;cells+=size;
  }
  return out;
}

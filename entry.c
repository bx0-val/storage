/* Keep Bend runtime flags separate from storage's command-line interface.
 * The UI is tiny; filesystem scanning uses its own bounded I/O worker pool. */
#include <stdlib.h>
int bend_runtime_main(int argc, char **argv);
int main(int argc, char **argv) {
  char **args=calloc((size_t)argc+5,sizeof(*args));
  if(!args)return 1;
  args[0]=argv[0];args[1]="--threads";args[2]="1";args[3]="--";
  for(int i=1;i<argc;i++)args[i+3]=argv[i];
  int result=bend_runtime_main(argc+3,args);
  free(args);return result;
}

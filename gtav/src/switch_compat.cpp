#include <pthread.h>

extern "C" int __real_pthread_detach(pthread_t thread);

extern "C" int __wrap_pthread_detach(pthread_t thread) {
  __real_pthread_detach(thread);
  return 0;
}

/*
 * First KLEE tutorial: testing a small function
 */
<<<<<<< HEAD
#include <klee/klee.h>
#include <stdio.h>
#include <string.h>


void testTaintPropagate(int i)
{
	klee_get_taint (&i, sizeof (i));
}


int main() {

  int key;
  klee_make_symbolic(&key, sizeof(key), "key");
  klee_set_taint(1, &key, sizeof(key));

  int clean = key;
  //int clean2;
  testTaintPropagate(clean);
  clean = ~key;

  //clean2 = clean + 2;
  testTaintPropagate(clean);
} 


=======

#include <klee/klee.h>

int get_sign(int x) {
  if (x == 0)
     return 0;
  
  if (x < 0)
     return -1;
  else 
     return 1;
} 

int main() {
  int a;
  klee_make_symbolic(&a, sizeof(a), "a");
  return get_sign(a);
} 
>>>>>>> 7a0f432e9f85652357efa8943fd0412c2b9e156b

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pt.h"

int main(void)
{
#ifndef NDEBUG
	PieceTable *pt;
	pt_print_struct_sizes();
	pt = pt_new_from_file("vector.c", 0, 0);
	pt_pprint(pt);
	assert(pt_size(pt) == 9616);
	pt_dbg("deletion: whole piece case\n");
	pt_erase(pt, 0, 9616);
	pt_pprint(pt);
	pt_free(pt);
	pt_dbg("deletion: end boundary case\n");
	pt = pt_new_from_file("vector.c", 0, 0);
	pt_erase(pt, 1, 1000000);
	pt_pprint(pt);
	pt_free(pt);
	pt_dbg("deletion: start boundary case\n");
	pt = pt_new_from_file("vector.c", 0, 0);
	pt_erase(pt, 0, 9615);
	pt_pprint(pt);
	pt_free(pt);
	pt_dbg("deletion: piece split case\n");
	pt = pt_new_from_file("vector.c", 0, 0);
	pt_erase(pt, 1, 9614);
	pt_pprint(pt);
	pt_free(pt);
	pt_dbg("deletion: multiple piece case general\n");
	pt = pt_new_from_file("vector.c", 0, 0);
	pt_insert(pt, 2, "XXX", 3);
	pt_erase(pt, 1, 9616);
	pt_pprint(pt);
	pt_free(pt);
	pt_dbg("deletion: multiple piece case start boundary\n");
	pt = pt_new_from_file("vector.c", 0, 0);
	pt_insert(pt, 1, "XXX", 3);
	pt_erase(pt, 1, 9616);
	pt_pprint(pt);
	pt_free(pt);
	pt_dbg("deletion: multiple piece case end boundary\n");
	pt = pt_new_from_file("vector.c", 0, 0);
	pt_insert(pt, 2, "XXX", 3);
	pt_erase(pt, 1, 1000000);
	pt_pprint(pt);
	pt_free(pt);
	pt_dbg("deletion: multiple piece case start+end boundaries\n");
	pt = pt_new_from_file("vector.c", 0, 0);
	pt_insert(pt, 2, "XXX", 3);
	pt_erase(pt, 0, 1000000);
	pt_pprint(pt);
	pt_free(pt);
#else
	struct timespec before, after;
	clock_gettime(CLOCK_REALTIME, &before);
	const char *xml = "test.xml";
	PieceTable *pt = pt_new_from_file(xml, 0, 0);
	printf("size: %ld\n", pt_size(pt));
	for(int i = 0; i < 100000; i++) {
		long n = 34+i*59;
		pt_erase(pt, n, 5);
		pt_insert(pt, n, "thang", 5);
	}
	clock_gettime(CLOCK_REALTIME, &after);
	printf("took: %f ms, final size: %ld\n",
			(after.tv_nsec - before.tv_nsec) / 1000000.0f +
			(after.tv_sec - before.tv_sec) * 1000,
			pt_size(pt));
	pt_pprint(pt);
#endif
}
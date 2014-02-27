#include <stdio.h>
#include <stdlib.h>

#include "rb_tree.h"

#define CHECK(C) ((C) ? (void)0 : check_error(#C, __FILE__,__LINE__))

static void check_error(const char* txt, const char* file, int line)
{
    fprintf(stderr, "CHECK(%s) FAILED at %s:%i\n", txt, file, line);
    abort();
}


typedef struct
{
    rb_tree_node rb_node;
    long key;
}Node;

int cmp(Node* a, Node* b)
{
    return a->key - b->key;
}

int cmp_key(Node* a, void* b_key)
{
    return a->key - (long)b_key;
}

void print_node(Node* node, int depth)
{
    static char spaces[] = "                                                  ";
    printf("%.*s%li\n", depth*2, spaces, node->key);
}

int main()
{
    rb_tree t;
    const long max = 1000;
    Node node[max];
    Node *p, *q;
    long i, j;

    for (i=0; i<max; i++) {
	node[i].key = i;
    }
    rb_tree_init(&t, (rb_tree_cmp_FT*)cmp,
		 (rb_tree_cmp_key_FT*)cmp_key,
		 (rb_tree_print_node_FT*)print_node);

    CHECK(!rb_tree_lookup_exact(&t, (void*)0));
    CHECK(!rb_tree_lookup_maxle(&t, (void*)0));
    CHECK(!rb_tree_min(&t));

    for (i=0; i<max; i += 10) {
	CHECK(rb_tree_insert(&t, &node[i].rb_node) == NULL);
	for (j=0; j<=i; j++) {
	    p = (Node*) rb_tree_lookup_exact(&t, (void*)j);
	    if (j % 10) {
		CHECK(!p);
	    }
	    else {
		CHECK(p);
		CHECK(p->key == j);
		q = (Node*) rb_tree_pred(&t, &p->rb_node);
		if (j > 0) {
		    CHECK(q);
		    CHECK(q->key == j - 10);
		    CHECK(rb_tree_succ(&t, &q->rb_node) == &p->rb_node);
		} else {
		    CHECK(!q);
		}
	    }
	    p = (Node*) rb_tree_lookup_maxle(&t, (void*)j);
	    CHECK(p);
	    CHECK(p->key == (j - j%10));
	    p = (Node*) rb_tree_min(&t);
	    CHECK(p);
	    CHECK(p->key == 0);
	}
    }

    for (i=5; i<max; i += 10) {
	CHECK(rb_tree_insert(&t, &node[i].rb_node) == NULL);
	for (j=0; j<=i; j++) {
	    p = (Node*) rb_tree_lookup_exact(&t, (void*)j);
	    if (j % 5) {
		CHECK(!p);
	    }
	    else {
		CHECK(p);
		CHECK(p->key == j);
		q = (Node*) rb_tree_pred(&t, &p->rb_node);
		if (j > 0) {
		    CHECK(q);
		    CHECK(q->key == j - 5);
		    CHECK(rb_tree_succ(&t, &q->rb_node) == &p->rb_node);
		} else {
		    CHECK(!q);
		}
	    }
	    p = (Node*) rb_tree_lookup_maxle(&t, (void*)j);
	    CHECK(p);
	    CHECK(p->key == (j - j%5));
	    p = (Node*) rb_tree_min(&t);
	    CHECK(p);
	    CHECK(p->key == 0);
	}
    }

    for (i=5; i<max; i += 10) {
	rb_tree_remove(&t, &node[i].rb_node);
	for (j=0; j<=i; j++) {
	    p = (Node*) rb_tree_lookup_exact(&t, (void*)j);
	    if (j % 10) {
		CHECK(!p);
	    }
	    else {
		CHECK(p);
		CHECK(p->key == j);
		q = (Node*) rb_tree_pred(&t, &p->rb_node);
		if (j > 0) {
		    CHECK(q);
		    CHECK(q->key == j - 10);
		    CHECK(rb_tree_succ(&t, &q->rb_node) == &p->rb_node);
		} else {
		    CHECK(!q);
		}
	    }
	    p = (Node*) rb_tree_lookup_maxle(&t, (void*)j);
	    CHECK(p);
	    CHECK(p->key == (j - j%10));
	    p = (Node*) rb_tree_min(&t);
	    CHECK(p);
	    CHECK(p->key == 0);
	}
    }
    rb_tree_print(&t);
}


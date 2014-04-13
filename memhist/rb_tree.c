#include "rb_tree.h"

#ifdef UNIT_TEST
#include <stdio.h>
#include <stdlib.h>
#  define ASSERT(C) ((C) ? (void)0 : assert_error(#C, __FILE__,__LINE__))

static void assert_error(const char* txt, const char* file, int line)
{
    fprintf(stderr, "ASSERT(%s) FAILED at %s:%i\n", txt, file, line);
    abort();
}

#else
#  include "pub_tool_libcassert.h"
#  define ASSERT(C) tl_assert(C)
#endif

/*
 * Initialize a new empty tree
 */
void rb_tree_init(rb_tree *newTree,
		  int  (*cmp)(rb_tree_node *a, rb_tree_node *b),
		  int  (*cmp_key)(rb_tree_node *a, void *b_key),
		  int  (*update_subtree)(rb_tree*, rb_tree_node*),
		  void (*print_node)(rb_tree_node *a, int depth))
{
    rb_tree_node *temp;

    newTree->cmp = cmp;
    newTree->cmp_key = cmp_key;
    newTree->update_subtree = update_subtree;
    newTree->print_node = print_node;

    /*  see the comment in the rb_tree structure in red_black_tree.h */
    /*  for information on nil and root */
    temp = &newTree->nil;
    temp->parent = temp->left = temp->right = temp;
    temp->red = 0;
    temp = &newTree->root;
    temp->parent = temp->left = temp->right = &newTree->nil;
    temp->red = 0;
}


static void left_rotate(rb_tree *tree, rb_tree_node *x)
{
    rb_tree_node *y;
    rb_tree_node *nil = &tree->nil;

    y = x->right;
    x->right = y->left;

    if (y->left != nil) y->left->parent = x;

    y->parent = x->parent;

    if (x == x->parent->left) {
	x->parent->left = y;
    } else {
	x->parent->right = y;
    }
    y->left = x;
    x->parent = y;

    tree->update_subtree(tree, x);
    tree->update_subtree(tree, y);
    ASSERT(!(y->parent != &tree->root
	     && tree->update_subtree(tree, y->parent)));

    ASSERT(!tree->nil.red);
}


static void right_rotate(rb_tree *tree, rb_tree_node *y)
{
    rb_tree_node *x;
    rb_tree_node *nil = &tree->nil;

    x = y->left;
    y->left = x->right;

    if (nil != x->right)  x->right->parent = y;

    x->parent = y->parent;

    if (y == y->parent->left) {
	y->parent->left = x;
    } else {
	y->parent->right = x;
    }
    x->right = y;
    y->parent = x;

    tree->update_subtree(tree, y);
    tree->update_subtree(tree, x);
    ASSERT(!(x->parent != &tree->root
	     && tree->update_subtree(tree, x->parent)));

    ASSERT(!tree->nil.red);
}



/* Help try insert as leaf node without any rebalancing
 *
 * RETURN: NULL if inserted otherwise clashing node
 */
static rb_tree_node* insert_helper(rb_tree *tree, rb_tree_node *z)
{
    rb_tree_node *x;
    rb_tree_node *y;
    rb_tree_node *nil = &tree->nil;
    int cmp = 1; /* to insert first node as root->left */

    z->left = z->right = nil;
    y = &tree->root;
    x = tree->root.left;
    while (x != nil) {
	y = x;
	cmp = tree->cmp(x, z);
	if (cmp > 0) { /* x.key > z.key */
	    x = x->left;
	} else if (cmp < 0) { /* x,key <= z.key */
	    x = x->right;
	} else
	    return x;
    }
    z->parent = y;
    if (cmp > 0) {
	y->left = z;
    } else {
	y->right = z;
    }

    ASSERT(!tree->nil.red);
    return NULL;
}


/* Try insert new node with unique key.
 *
 * RETURN:  NULL if inserted otherwise clashing node
 */
rb_tree_node* rb_tree_insert(rb_tree *tree, rb_tree_node *x)
{
    rb_tree_node *y;
    rb_tree_node *clash;

    clash = insert_helper(tree, x);
    if (clash) return clash;
    x->red = 1;

    ASSERT(x->left == &tree->nil && x->right == &tree->nil);
    for (y = x->parent; y != &tree->root; y = y->parent) {
	if (!tree->update_subtree(tree, y))
	    break;
    }
    while (x->parent->red) { /* use sentinel instead of checking for root */
	if (x->parent == x->parent->parent->left) {
	    y = x->parent->parent->right;
	    if (y->red) {
		x->parent->red = 0;
		y->red = 0;
		x->parent->parent->red = 1;
		x = x->parent->parent;
	    } else {
		if (x == x->parent->right) {
		    x = x->parent;
		    left_rotate(tree, x);
		}
		x->parent->red = 0;
		x->parent->parent->red = 1;
		right_rotate(tree, x->parent->parent);
	    }
	} else { /* case for x->parent == x->parent->parent->right */
	    y = x->parent->parent->left;
	    if (y->red) {
		x->parent->red = 0;
		y->red = 0;
		x->parent->parent->red = 1;
		x = x->parent->parent;
	    } else {
		if (x == x->parent->left) {
		    x = x->parent;
		    right_rotate(tree, x);
		}
		x->parent->red = 0;
		x->parent->parent->red = 1;
		left_rotate(tree, x->parent->parent);
	    }
	}
    }
    tree->root.left->red = 0;

    ASSERT(!tree->nil.red);
    ASSERT(!tree->root.red);
    return NULL;
}


/* Return node with the minimum key
*/
rb_tree_node* rb_tree_min(rb_tree *tree)
{
    rb_tree_node *x;
    rb_tree_node *min = NULL;
    rb_tree_node *nil = &tree->nil;

    x = tree->root.left;
    while (x != nil) {
	min = x;
	x = x->left;
    }
    return min;
}



/* Return the successor of x or NULL if no successor exists.
 */
rb_tree_node* rb_tree_succ(rb_tree *tree, rb_tree_node *x)
{
    rb_tree_node *y;
    rb_tree_node *nil  = &tree->nil;
    rb_tree_node *root = &tree->root;

    y = x->right;
    if (y != nil) {
	while (y->left != nil) { /* returns the minium of the right subtree of x */
	    y = y->left;
	}
	return y;
    } else {
	y = x->parent;
	while (x == y->right) { /* sentinel used instead of checking for nil */
	    x = y;
	    y = y->parent;
	}
	return (y == root) ? NULL : y;
    }
}

/* Return the predecessor of x or NULL if no predecessor exists.
 */
rb_tree_node* rb_tree_pred(rb_tree *tree, rb_tree_node *x)
{
    rb_tree_node *y;
    rb_tree_node *nil  = &tree->nil;
    rb_tree_node *root = &tree->root;

    y = x->left;
    if (y != nil) {
	while (y->right != nil) { /* returns the maximum of the left subtree of x */
	    y = y->right;
	}
	return y;
    } else {
	y = x->parent;
	while (x == y->left) {
	    if (y == root) return NULL;
	    x = y;
	    y = y->parent;
	}
	return y;
    }
}

static void inorder_print(rb_tree *tree, rb_tree_node *x, int depth)
{
    if (x != &tree->nil) {
	inorder_print(tree, x->left, depth + 1);
	tree->print_node(x, depth);
	inorder_print(tree, x->right, depth + 1);
    }
}

/* Print entire tree
 */
void rb_tree_print(rb_tree *tree) {
    inorder_print(tree, tree->root.left, 0);
}


/* Return node with key equal to 'key'
 * or NULL if no such node exist.
 */
rb_tree_node* rb_tree_lookup_exact(rb_tree *tree, void *key)
{
    rb_tree_node *x = tree->root.left;
    rb_tree_node *nil = &tree->nil;
    int cmp;

    for (;;) {
	if (x == nil)
	    return NULL;
	cmp = tree->cmp_key(x, key);
	if (cmp > 0) /* x->key > key */
	    x = x->left;
	else if (cmp < 0)
	    x = x->right;
	else
	    break;
    }
    return x;
}

/* Return node with the maximum key that is less or equal to 'key'
 * or NULL if no such node exist.
 */
rb_tree_node* rb_tree_lookup_maxle(rb_tree *tree, void *key)
{
    rb_tree_node *x = tree->root.left;
    rb_tree_node *nil = &tree->nil;
    rb_tree_node *maxless = NULL;
    int cmp;

    while (x != nil) {
	cmp = tree->cmp_key(x, key);
	if (cmp > 0) { /* x->key > key */
	    x = x->left;
	} else if (cmp < 0) {
	    maxless = x;
	    x = x->right;
	} else
	    return x;
    }
    return maxless;
}

/* Return node with the minimum key that is greater than 'key'
 * or NULL if no such node exist.
 */
rb_tree_node* rb_tree_lookup_ming(rb_tree *tree, void *key)
{
    rb_tree_node *x = tree->root.left;
    rb_tree_node *nil = &tree->nil;
    rb_tree_node *ming = NULL;
    int cmp;

    while (x != nil) {
	cmp = tree->cmp_key(x, key);
	if (cmp > 0) { /* x->key > key */
	    ming = x;
	    x = x->left;
	} else if (cmp <= 0) {
	    x = x->right;
	}
    }
    return ming;
}


/* Perform rotations and changes colors to restore red-black
 * properties after a node is deleted.
 * 'x' is the child of the spliced out node.
 */
static void remove_fixup(rb_tree *tree, rb_tree_node *x)
{
    rb_tree_node *root = tree->root.left;
    rb_tree_node *w;

    while ((!x->red) && (root != x)) {
	if (x == x->parent->left) {
	    w = x->parent->right;
	    if (w->red) {
		w->red = 0;
		x->parent->red = 1;
		left_rotate(tree, x->parent);
		w = x->parent->right;
	    }
	    if ((!w->right->red) && (!w->left->red)) {
		w->red = 1;
		x = x->parent;
	    } else {
		if (!w->right->red) {
		    w->left->red = 0;
		    w->red = 1;
		    right_rotate(tree, w);
		    w = x->parent->right;
		}
		w->red = x->parent->red;
		x->parent->red = 0;
		w->right->red = 0;
		left_rotate(tree, x->parent);
		x = root; /* this is to exit while loop */
	    }
	} else { /* the code below is has left and right switched from above */
	    w = x->parent->left;
	    if (w->red) {
		w->red = 0;
		x->parent->red = 1;
		right_rotate(tree, x->parent);
		w = x->parent->left;
	    }
	    if ((!w->right->red) && (!w->left->red)) {
		w->red = 1;
		x = x->parent;
	    } else {
		if (!w->left->red) {
		    w->right->red = 0;
		    w->red = 1;
		    left_rotate(tree, w);
		    w = x->parent->left;
		}
		w->red = x->parent->red;
		x->parent->red = 0;
		w->left->red = 0;
		right_rotate(tree, x->parent);
		x = root; /* this is to exit while loop */
	    }
	}
    }
    x->red = 0;

    ASSERT(!tree->nil.red);
}


/* Remove node from tree.
 * The node must exist in the tree.
 */
void rb_tree_remove(rb_tree *tree, rb_tree_node *z)
{
    rb_tree_node *y;
    rb_tree_node *x;
    rb_tree_node *nil =  &tree->nil;
    rb_tree_node *root = &tree->root;
    rb_tree_node *p;

    y = ((z->left == nil) || (z->right == nil)) ? z : rb_tree_succ(tree, z);
    x = (y->left == nil) ? y->right : y->left;
    x->parent = y->parent;
    if (root == x->parent) {
	root->left = x;
    } else {
	if (y == y->parent->left) {
	    y->parent->left = x;
	} else {
	    y->parent->right = x;
	}
    }

    for (p = x->parent; p != root; p = p->parent) {
	if (!tree->update_subtree(tree, p))
	    break;
    }

    if (y != z) { /* y should not be nil in this case */

	ASSERT(y != &tree->nil);

	/* y is the node to splice out and x is its child */

	if (!(y->red)) remove_fixup(tree, x);

	y->left = z->left;
	y->right = z->right;
	y->parent = z->parent;
	y->red = z->red;
	z->left->parent = z->right->parent = y;
	if (z == z->parent->left) {
	    z->parent->left = y;
	} else {
	    z->parent->right = y;
	}

	for (p = y; p != root; p = p->parent) {
	    if (!tree->update_subtree(tree, p))
		break;
	}
    }
    else {
	if (!(y->red)) remove_fixup(tree, x);
    }

    ASSERT(!tree->nil.red);
}


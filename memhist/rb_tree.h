typedef struct rb_tree rb_tree;
typedef struct rb_tree_node rb_tree_node;

/*
 * Callbacks that an implementator need to supply:
 */

/* cmp and cmp_key should return >0 if a > b, <0 if a < b, and 0 if equal */
typedef int rb_tree_cmp_FT(rb_tree_node* a, rb_tree_node* b);
typedef int rb_tree_cmp_key_FT(rb_tree_node* a, void* b_key);

/* update_subtree should update node information about the entire sub-tree.
   Return false if node was unchanged, i.e no updates need to propagate further
   towards root.
 */
typedef int rb_tree_update_subtree_FT(rb_tree*, rb_tree_node*, int do_update);

/* print tree node (indented with 'depth' on one line)
*/
typedef void rb_tree_print_node_FT(rb_tree_node*, int depth);


typedef struct rb_tree_node
{
  int red;
  struct rb_tree_node* left;
  struct rb_tree_node* right;
  struct rb_tree_node* parent;
} rb_tree_node;


typedef struct rb_tree
{
    rb_tree_cmp_FT* cmp;
    rb_tree_cmp_key_FT* cmp_key;
    rb_tree_update_subtree_FT* update_subtree;
    rb_tree_print_node_FT* print_node;

    /*  Sentinels used for root and for nil.
        root.left should always point to the node which is the root of the tree.
        nil should always be black but has aribtrary children and parent.
        The point of using these sentinels is so that the root and nil nodes
        do not require special cases in the code */
    rb_tree_node root;
    rb_tree_node nil;
} rb_tree;



void rb_tree_init(rb_tree* newTree,
		  rb_tree_cmp_FT*,
		  rb_tree_cmp_key_FT*,
		  rb_tree_update_subtree_FT*,
		  rb_tree_print_node_FT*);
rb_tree_node * rb_tree_insert(rb_tree*, rb_tree_node*);
void rb_tree_print(rb_tree*);
void rb_tree_remove(rb_tree*, rb_tree_node*);
rb_tree_node* rb_tree_min(rb_tree* tree);
rb_tree_node* rb_tree_pred(rb_tree*,rb_tree_node*);
rb_tree_node* rb_tree_succ(rb_tree*,rb_tree_node*);
rb_tree_node* rb_tree_lookup_exact(rb_tree*, void*);
rb_tree_node* rb_tree_lookup_maxle(rb_tree* tree, void* key);
rb_tree_node* rb_tree_lookup_ming(rb_tree* tree, void* key);
void rb_tree_node_updated(rb_tree*, rb_tree_node*);
void rb_tree_check(rb_tree* tree, rb_tree_node* offender);

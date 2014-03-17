
typedef struct rb_tree_node
{
  int red;
  struct rb_tree_node* left;
  struct rb_tree_node* right;
  struct rb_tree_node* parent;
} rb_tree_node;


typedef struct rb_tree
{
    /* cmp and cmp_key should return >0 if a > b, <0 if a < b, and 0 if equal */
    int  (*cmp)(rb_tree_node* a, rb_tree_node* b);
    int  (*cmp_key)(rb_tree_node* a, void* b_key);
    void (*print_node)(rb_tree_node*, int depth);

    /*  Sentinels used for root and for nil.
        root.left should always point to the node which is the root of the tree.
        nil should always be black but has aribtrary children and parent.
        The point of using these sentinels is so that the root and nil nodes
        do not require special cases in the code */
    rb_tree_node root;
    rb_tree_node nil;
} rb_tree;

typedef int rb_tree_cmp_FT(rb_tree_node* a, rb_tree_node* b);
typedef int rb_tree_cmp_key_FT(rb_tree_node* a, void* b_key);
typedef void rb_tree_print_node_FT(rb_tree_node*, int depth);

void rb_tree_init(rb_tree* newTree,
		  rb_tree_cmp_FT*,
		  rb_tree_cmp_key_FT*,
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


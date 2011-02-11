#include "btree/internal_node.hpp"
#include <algorithm>
#include "utils.hpp"

//#define DEBUG_MAX_INTERNAL 10

//In this tree, less than or equal takes the left-hand branch and greater than takes the right hand branch

namespace internal_node {

using namespace impl;

void init(block_size_t block_size, internal_node_t *node) {
    node->magic = internal_node_t::expected_magic;
    node->npairs = 0;
    node->frontmost_offset = block_size.value();
}

void init(block_size_t block_size, internal_node_t *node, const internal_node_t *lnode, const uint16_t *offsets, int numpairs) {
    init(block_size, node);
    for (int i = 0; i < numpairs; i++) {
        node->pair_offsets[i] = insert_pair(node, get_pair(lnode, offsets[i]));
    }
    node->npairs = numpairs;
    std::sort(node->pair_offsets, node->pair_offsets+node->npairs, internal_key_comp(node));
}

block_id_t lookup(const internal_node_t *node, const btree_key_t *key) {
    int index = get_offset_index(node, key);
#ifdef BTREE_DEBUG
    printf("Look up:");
    key->print();
    printf("\n");
    internal_node::print(node);
    printf("\t");
    for (int i = 0; i < index; i++)
        printf("\t\t");
    printf("|\n");
    printf("\t");
    for (int i = 0; i < index; i++)
        printf("\t\t");
    printf("V\n");
#endif
    return get_pair_by_index(node, index)->lnode;
}

bool insert(block_size_t block_size, internal_node_t *node, const btree_key_t *key, block_id_t lnode, block_id_t rnode) {
    //TODO: write a unit test for this
    rassert(key->size <= MAX_KEY_SIZE, "key too large");
    if (is_full(node)) return false;
    if (node->npairs == 0) {
        btree_key_t special;
        special.size = 0;

        uint16_t special_offset = insert_pair(node, rnode, &special);
        insert_offset(node, special_offset, 0);
    }

    int index = get_offset_index(node, key);
    rassert(!is_equal(&get_pair_by_index(node, index)->key, key),
        "tried to insert duplicate key into internal node!");
    uint16_t offset = insert_pair(node, lnode, key);
    insert_offset(node, offset, index);

    get_pair_by_index(node, index+1)->lnode = rnode;
    return true;
}

bool remove(block_size_t block_size, internal_node_t *node, const btree_key_t *key) {
#ifdef BTREE_DEBUG
    printf("removing key\n");
    internal_node::print(node);
#endif
    int index = get_offset_index(node, key);
    delete_pair(node, node->pair_offsets[index]);
    delete_offset(node, index);

    if (index == node->npairs)
        make_last_pair_special(node);
#ifdef BTREE_DEBUG
    printf("\t|\n\t|\n\t|\n\tV\n");
    internal_node::print(node);
#endif

    validate(block_size, node);
    return true;
}

void split(block_size_t block_size, internal_node_t *node, internal_node_t *rnode, btree_key_t *median) {
#ifdef BTREE_DEBUG
    printf("splitting key\n");
    internal_node::print(node);
#endif
    uint16_t total_pairs = block_size.value() - node->frontmost_offset;
    uint16_t first_pairs = 0;
    int index = 0;
    while (first_pairs < total_pairs/2) { // finds the median index
        first_pairs += pair_size(get_pair_by_index(node, index));
        index++;
    }
    int median_index = index;

    // Equality takes the left branch, so the median should be from this node.
    const btree_key_t *median_key = &get_pair_by_index(node, median_index-1)->key;
    keycpy(median, median_key);

    init(block_size, rnode, node, node->pair_offsets + median_index, node->npairs - median_index);

    // TODO: This is really slow because most pairs will likely be copied
    // repeatedly.  There should be a better way.
    for (index = median_index; index < node->npairs; index++) {
        delete_pair(node, node->pair_offsets[index]);
    }

    node->npairs = median_index;
    //make last pair special
    make_last_pair_special(node);
#ifdef BTREE_DEBUG
    printf("\t|\n\t|\n\t|\n\tV\n");
    internal_node::print(node);
#endif
    validate(block_size, node);
    validate(block_size, rnode);
}

void merge(block_size_t block_size, const internal_node_t *node, internal_node_t *rnode, btree_key_t *key_to_remove, internal_node_t *parent) {
#ifdef BTREE_DEBUG
    printf("merging\n");
    printf("node:\n");
    internal_node::print(node);
    printf("rnode:\n");
    internal_node::print(rnode);
#endif
    validate(block_size, node);
    validate(block_size, rnode);
    // get the key in parent which points to node
    const btree_key_t *key_from_parent = &get_pair_by_index(parent, get_offset_index(parent, &get_pair_by_index(node, 0)->key))->key;

    guarantee(sizeof(internal_node_t) + (node->npairs + rnode->npairs)*sizeof(*node->pair_offsets) +
        (block_size.value() - node->frontmost_offset) + (block_size.value() - rnode->frontmost_offset) + key_from_parent->size < block_size.value(),
        "internal nodes too full to merge");

    memmove(rnode->pair_offsets + node->npairs, rnode->pair_offsets, rnode->npairs * sizeof(*rnode->pair_offsets));

    for (int i = 0; i < node->npairs-1; i++) { // the last pair is special
        rnode->pair_offsets[i] = insert_pair(rnode, get_pair_by_index(node, i));
    }
    rnode->pair_offsets[node->npairs-1] = insert_pair(rnode, get_pair_by_index(node, node->npairs-1)->lnode, key_from_parent);
    rnode->npairs += node->npairs;

    keycpy(key_to_remove, &get_pair_by_index(rnode, 0)->key);
#ifdef BTREE_DEBUG
    printf("\t|\n\t|\n\t|\n\tV\n");
    printf("node:\n");
    internal_node::print(node);
    printf("rnode:\n");
    internal_node::print(rnode);
#endif
    validate(block_size, rnode);
}

bool level(block_size_t block_size, internal_node_t *node, internal_node_t *sibling, btree_key_t *key_to_replace, btree_key_t *replacement_key, internal_node_t *parent) {
    validate(block_size, node);
    validate(block_size, sibling);
#ifdef BTREE_DEBUG
    printf("levelling\n");
    printf("node:\n");
    internal_node::print(node);
    printf("sibling:\n");
    internal_node::print(sibling);
#endif

    if (nodecmp(node, sibling) < 0) {
        const btree_key_t *key_from_parent = &get_pair_by_index(parent, get_offset_index(parent, &get_pair_by_index(node, 0)->key))->key;
        if (sizeof(internal_node_t) + (node->npairs + 1) * sizeof(*node->pair_offsets) + pair_size_with_key(key_from_parent) >= node->frontmost_offset)
            return false;
        uint16_t special_pair_offset = node->pair_offsets[node->npairs-1];
        block_id_t last_offset = get_pair(node, special_pair_offset)->lnode;
        node->pair_offsets[node->npairs-1] = insert_pair(node, last_offset, key_from_parent);

        // TODO: This loop involves repeated memmoves.  There should be a way to drastically reduce the number and increase efficiency.
        while (true) { // TODO: find cleaner way to construct loop
            const btree_internal_pair *pair_to_move = get_pair_by_index(sibling, 0);
            uint16_t size_change = sizeof(*node->pair_offsets) + pair_size(pair_to_move);
            if (node->npairs*sizeof(*node->pair_offsets) + (block_size.value() - node->frontmost_offset) + size_change >= sibling->npairs*sizeof(*sibling->pair_offsets) + (block_size.value() - sibling->frontmost_offset) - size_change)
                break;
            node->pair_offsets[node->npairs++] = insert_pair(node, pair_to_move);
            delete_pair(sibling, sibling->pair_offsets[0]);
            delete_offset(sibling, 0);
        }

        const btree_internal_pair *pair_for_parent = get_pair_by_index(sibling, 0);
        node->pair_offsets[node->npairs++] = special_pair_offset;
        get_pair(node, special_pair_offset)->lnode = pair_for_parent->lnode;

        keycpy(key_to_replace, &get_pair_by_index(node, 0)->key);
        keycpy(replacement_key, &pair_for_parent->key);

        delete_pair(sibling, sibling->pair_offsets[0]);
        delete_offset(sibling, 0);
    } else {
        uint16_t offset;
        const btree_key_t *key_from_parent = &get_pair_by_index(parent, get_offset_index(parent, &get_pair_by_index(sibling, 0)->key))->key;
        if (sizeof(internal_node_t) + (node->npairs + 1) * sizeof(*node->pair_offsets) + pair_size_with_key(key_from_parent) >= node->frontmost_offset)
            return false;
        block_id_t first_offset = get_pair_by_index(sibling, sibling->npairs-1)->lnode;
        offset = insert_pair(node, first_offset, key_from_parent);
        insert_offset(node, offset, 0);
        delete_pair(sibling, sibling->pair_offsets[sibling->npairs-1]);
        delete_offset(sibling, sibling->npairs-1);

        // TODO: This loop involves repeated memmoves.  There should be a way to drastically reduce the number and increase efficiency.
        while (true) { // TODO: find cleaner way to construct loop
            const btree_internal_pair *pair_to_move = get_pair_by_index(sibling, sibling->npairs-1);
            uint16_t size_change = sizeof(*node->pair_offsets) + pair_size(pair_to_move);
            if (node->npairs*sizeof(*node->pair_offsets) + (block_size.value() - node->frontmost_offset) + size_change >= sibling->npairs*sizeof(*sibling->pair_offsets) + (block_size.value() - sibling->frontmost_offset) - size_change)
                break;
            offset = insert_pair(node, pair_to_move);
            insert_offset(node, offset, 0);

            delete_pair(sibling, sibling->pair_offsets[sibling->npairs-1]);
            delete_offset(sibling, sibling->npairs-1);
        }

        keycpy(key_to_replace, &get_pair_by_index(sibling, 0)->key);
        keycpy(replacement_key, &get_pair_by_index(sibling, sibling->npairs-1)->key);

        make_last_pair_special(sibling);
    }

#ifdef BTREE_DEBUG
    printf("\t|\n\t|\n\t|\n\tV\n");
    printf("node:\n");
    internal_node::print(node);
    printf("sibling:\n");
    internal_node::print(sibling);
#endif
    validate(block_size, node);
    validate(block_size, sibling);
    guarantee(!change_unsafe(node), "level made internal node dangerously full");
    return true;
}

int sibling(const internal_node_t *node, const btree_key_t *key, block_id_t *sib_id) {
    int index = get_offset_index(node, key);
    const btree_internal_pair *sib_pair;
    int cmp;
    if (index > 0) {
        sib_pair = get_pair_by_index(node, index-1);
        cmp = 1;
    } else {
        sib_pair = get_pair_by_index(node, index+1);
        cmp = -1;
    }

    *sib_id = sib_pair->lnode;
    return cmp; //equivalent to nodecmp(node, sibling)
}

void update_key(internal_node_t *node, const btree_key_t *key_to_replace, const btree_key_t *replacement_key) {
#ifdef BTREE_DEBUG
    printf("updating key\n");
    internal_node::print(node);
#endif
    int index = get_offset_index(node, key_to_replace);
    block_id_t tmp_lnode = get_pair_by_index(node, index)->lnode;
    delete_pair(node, node->pair_offsets[index]);

    guarantee(sizeof(internal_node_t) + (node->npairs) * sizeof(*node->pair_offsets) + pair_size_with_key(replacement_key) < node->frontmost_offset,
        "cannot fit updated key in internal node");

    node->pair_offsets[index] = insert_pair(node, tmp_lnode, replacement_key);
#ifdef BTREE_DEBUG
    printf("\t|\n\t|\n\t|\n\tV\n");
    internal_node::print(node);
#endif

    rassert(is_sorted(node->pair_offsets, node->pair_offsets+node->npairs, internal_key_comp(node)),
            "Invalid key given to update_key: offsets no longer in sorted order");
}

bool is_full(const internal_node_t *node) {
#ifdef DEBUG_MAX_INTERNAL
    if (node->npairs-1 >= DEBUG_MAX_INTERNAL)
        return true;
#endif
#ifdef BTREE_DEBUG
    printf("sizeof(internal_node_t): %ld, (node->npairs + 1): %d, sizeof(*node->pair_offsets): %ld, sizeof(internal_node_t): %ld, MAX_KEY_SIZE: %d, node->frontmost_offset: %d\n", sizeof(internal_node_t), (node->npairs + 1), sizeof(*node->pair_offsets), sizeof(btree_internal_pair), MAX_KEY_SIZE, node->frontmost_offset);
#endif
    return sizeof(internal_node_t) + (node->npairs + 1) * sizeof(*node->pair_offsets) + pair_size_with_key_size(MAX_KEY_SIZE) >=  node->frontmost_offset;
}

bool change_unsafe(const internal_node_t *node) {
#ifdef DEBUG_MAX_INTERNAL
    if (node->npairs-1 >= DEBUG_MAX_INTERNAL)
        return true;
#endif
    return sizeof(internal_node_t) + node->npairs * sizeof(*node->pair_offsets) + MAX_KEY_SIZE >= node->frontmost_offset;
}

void validate(block_size_t block_size, const internal_node_t *node) {
#ifndef NDEBUG
    rassert(ptr_cast<byte>(&(node->pair_offsets[node->npairs])) <= ptr_cast<byte>(get_pair(node, node->frontmost_offset)));
    rassert(node->frontmost_offset > 0);
    rassert(node->frontmost_offset <= block_size.value());
    for (int i = 0; i < node->npairs; i++) {
        rassert(node->pair_offsets[i] < block_size.value());
        rassert(node->pair_offsets[i] >= node->frontmost_offset);
    }
    rassert(is_sorted(node->pair_offsets, node->pair_offsets+node->npairs, internal_key_comp(node)),
        "Offsets no longer in sorted order");
#endif
}

bool is_underfull(block_size_t block_size, const internal_node_t *node) {
#ifdef DEBUG_MAX_INTERNAL
    return node->npairs < (DEBUG_MAX_INTERNAL + 1) / 2;
#endif
    return (sizeof(internal_node_t) + 1) / 2 +
        node->npairs*sizeof(*node->pair_offsets) +
        (block_size.value() - node->frontmost_offset) +
        /* EPSILON TODO this epsilon is too high lower it*/
        INTERNAL_EPSILON * 2  < block_size.value() / 2;
}

bool is_mergable(block_size_t block_size, const internal_node_t *node, const internal_node_t *sibling, const internal_node_t *parent) {
#ifdef DEBUG_MAX_INTERNAL
    return node->npairs + sibling->npairs < DEBUG_MAX_INTERNAL;
#endif
    const btree_key_t *key_from_parent;
    if (nodecmp(node, sibling) < 0) {
        key_from_parent = &get_pair_by_index(parent, get_offset_index(parent, &get_pair_by_index(node, 0)->key))->key;
    } else {
        key_from_parent = &get_pair_by_index(parent, get_offset_index(parent, &get_pair_by_index(sibling, 0)->key))->key;
    }
    return sizeof(internal_node_t) +
        (node->npairs + sibling->npairs + 1)*sizeof(*node->pair_offsets) +
        (block_size.value() - node->frontmost_offset) +
        (block_size.value() - sibling->frontmost_offset) + key_from_parent->size +
        pair_size_with_key_size(MAX_KEY_SIZE) +
        INTERNAL_EPSILON < block_size.value(); // must still have enough room for an arbitrary key  // TODO: we can't be tighter?
}

bool is_singleton(const internal_node_t *node) {
    return node->npairs == 2;
}

size_t pair_size(const btree_internal_pair *pair) {
    return pair_size_with_key_size(pair->key.size);
}

const btree_internal_pair *get_pair(const internal_node_t *node, uint16_t offset) {
    return ptr_cast<btree_internal_pair>(ptr_cast<byte>(node) + offset);
}

btree_internal_pair *get_pair(internal_node_t *node, uint16_t offset) {
    return ptr_cast<btree_internal_pair>(ptr_cast<byte>(node) + offset);
}

const btree_internal_pair *get_pair_by_index(const internal_node_t *node, int index) {
    return get_pair(node, node->pair_offsets[index]);
}
btree_internal_pair *get_pair_by_index(internal_node_t *node, int index) {
    return get_pair(node, node->pair_offsets[index]);
}

int nodecmp(const internal_node_t *node1, const internal_node_t *node2) {
    const btree_key_t *key1 = &get_pair_by_index(node1, 0)->key;
    const btree_key_t *key2 = &get_pair_by_index(node2, 0)->key;

    return sized_strcmp(key1->contents, key1->size, key2->contents, key2->size);
}

void print(const internal_node_t *node) {
    int freespace = node->frontmost_offset - (sizeof(internal_node_t) + (node->npairs + 1) * sizeof(*node->pair_offsets) + sizeof(btree_internal_pair) + MAX_KEY_SIZE);
    printf("Free space in node: %d\n", freespace);
    for (int i = 0; i < node->npairs; i++) {
        const btree_internal_pair *pair = get_pair_by_index(node, i);
        printf("|\t");
        pair->key.print();
        printf("\t");
    }
    printf("|\n");
    for (int i = 0; i < node->npairs; i++) {
        printf("|\t");
        printf(".");
        printf("\t");
    }
    printf("|\n");
}

namespace impl {

size_t pair_size_with_key(const btree_key_t *key) {
    return pair_size_with_key_size(key->size);
}

size_t pair_size_with_key_size(uint8_t size) {
    return offsetof(btree_internal_pair, key) + offsetof(btree_key_t, contents) + size;
}

void delete_pair(internal_node_t *node, uint16_t offset) {
    btree_internal_pair *pair_to_delete = get_pair(node, offset);
    btree_internal_pair *front_pair = get_pair(node, node->frontmost_offset);
    size_t shift = pair_size(pair_to_delete);
    size_t size = offset - node->frontmost_offset;

    rassert(check_magic<node_t>(node->magic));
    memmove(ptr_cast<byte>(front_pair)+shift, front_pair, size);
    rassert(check_magic<node_t>(node->magic));

    node->frontmost_offset += shift;
    for (int i = 0; i < node->npairs; i++) {
        if (node->pair_offsets[i] < offset)
            node->pair_offsets[i] += shift;
    }
}

uint16_t insert_pair(internal_node_t *node, const btree_internal_pair *pair) {
    node->frontmost_offset -= pair_size(pair);
    btree_internal_pair *new_pair = get_pair(node, node->frontmost_offset);

    // insert contents
    memcpy(new_pair, pair, pair_size(pair));

    return node->frontmost_offset;
}

uint16_t insert_pair(internal_node_t *node, block_id_t lnode, const btree_key_t *key) {
    node->frontmost_offset -= pair_size_with_key(key);
    btree_internal_pair *new_pair = get_pair(node, node->frontmost_offset);

    // insert contents
    new_pair->lnode = lnode;
    keycpy(&new_pair->key, key);

    return node->frontmost_offset;
}

int get_offset_index(const internal_node_t *node, const btree_key_t *key) {
    return std::lower_bound(node->pair_offsets, node->pair_offsets+node->npairs, (uint16_t) internal_key_comp::faux_offset, internal_key_comp(node, key)) - node->pair_offsets;
}

void delete_offset(internal_node_t *node, int index) {
    uint16_t *pair_offsets = node->pair_offsets;
    if (node->npairs > 1)
        memmove(pair_offsets+index, pair_offsets+index+1, (node->npairs-index-1) * sizeof(uint16_t));
    node->npairs--;
}

void insert_offset(internal_node_t *node, uint16_t offset, int index) {
    uint16_t *pair_offsets = node->pair_offsets;
    memmove(pair_offsets+index+1, pair_offsets+index, (node->npairs-index) * sizeof(uint16_t));
    pair_offsets[index] = offset;
    node->npairs++;
}

void make_last_pair_special(internal_node_t *node) {
    int index = node->npairs-1;
    uint16_t old_offset = node->pair_offsets[index];
    btree_key_t tmp;
    tmp.size = 0;
    node->pair_offsets[index] = insert_pair(node, get_pair(node, old_offset)->lnode, &tmp);
    delete_pair(node, old_offset);
}


bool is_equal(const btree_key_t *key1, const btree_key_t *key2) {
    return sized_strcmp(key1->contents, key1->size, key2->contents, key2->size) == 0;
}

}  // namespace internal_node::impl

}  // namespace internal_node

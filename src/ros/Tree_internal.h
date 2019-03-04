/** \file Tree_internal.h
 * \author Patrick MacArthur
 *
 * Declare B-tree node class (internal to B-tree implementation).
 */

#ifndef TREE_INTERNAL_H
#define TREE_INTERNAL_H

#include <cstdint>
#include <iosfwd>

/** Store a single node of a B-tree.
 *
 * Nodes satisfy the following invariants:
 *   - The node must have at least one key (i.e., \p n must be at least 1).
 *   - The node may have at most \p valueCount keys.
 *   - All leaf nodes in the tree must have the same height.
 *   - A node must either have 0 children (i.e., the node is a leaf node) or \p
 *   n + 1 children (i.e., the node is an internal node).
 *   - Keys are stored in a node in increasing order.
 *   - For every child \c i, all keys stored in <tt>child[i]</tt> are strictly
 *   less than <tt>key[i]</tt> and strictly greater than <tt>key[i-1]</tt> (for
 *   \c i > 0).
 */
template <typename T>
struct Node {
    uint8_t valueCount;
        /**< Maximum number of keys for a B tree of degree \p N. */
    uint8_t childCount;
        /**< Maximum number of children for a B tree of degree \p N. */

    uint16_t n;
        /**< Number of keys. Must be in range 1..\p valueCount. */
    T *key;
        /**< Keys stored in this node. Only values 0..\p n -1 are valid. */
    struct ChildEntry {
        Node<T> *ptr;
        uint32_t minVersion;
        uint32_t maxVersion;
    } *child;
        /**< Child nodes of this node. Only values 0..\p n are valid. */

    Node(Node<T> *leftChild, const T &v, Node<T> *rightChild,
         int maxChildCount);
    ~Node();
    Node(const Node &node) = delete;
    Node &operator =(const Node &node) = delete;

    /** \brief Construct a node from the given node.
     *
     * Construct a node from the given node, destroying \p node in the process.
     * The passed-in node \p node is in an undefined state after this function
     * is called and should not be referenced in any way after this function has
     * returned.
     */
    Node(Node &&node) = delete;

    /** \brief Replace this node with the contents of \p node.
     *
     * Assign this node the contents of \p node. The passed-in node \p node is
     * in an undefined state after this function is called and should not be
     * referenced in any way after this function has returned.
     */
    Node &operator =(Node &&) = delete;

    bool exists(const T &v, unsigned long version) const;
    bool leaf() const { return !this->child[0].ptr; }
    int height() const;
    bool insert(T &v, Node<T> *&newNode, unsigned long version);
    static void erase(Node *&node, const T &v, unsigned long version);
    std::ostream &output(std::ostream &os, int level, unsigned long version) const;
    void check_invariants(T, const T &) const;

    static constexpr size_t size() { return sizeof(Node); }// + valueCount * sizeof
#if 0
    void *operator new(size_t size);
    void operator delete(void *p);
#endif

private:
    void add_item(int pos, const T &v, Node<T> *right, unsigned long version);
    void split(int pos, T &v, Node<T> *&newNode, unsigned long version);

    T remove_item(int pos, unsigned long version);

    /** \brief Move v and all items from right into left. */
    void merge(Node *left, int v, Node *right);
};

extern template class Node<int>;

#endif

/** \file Tree.cc
 * \author Patrick MacArthur
 *
 * This file contains the implementation of the 2-3 tree class.
 */

#include "Tree.h"
#include "Tree_internal.h"

#include <iostream>
#include <limits>
#include <utility>
#include <cassert>

/** Construct an empty tree. */
template <typename T, int N>
Tree<T, N>::Tree() {
    _root = NULL;
}

/** Destroy a tree. */
template <typename T, int N>
Tree<T, N>::~Tree() {
    delete _root;
}

/** Return true if and only if the tree is empty. */
template <typename T, int N>
bool Tree<T, N>::isEmpty() const {
    return !_root;
}

/** Destroy the node and its children.  */
template <typename T, int N>
Node<T, N>::~Node() {
    for (int i = 0; i <= this->n; ++i) {
        delete this->child[i].ptr;
    }
}

/** Return true if and only if the value \p v exists in the tree. */
template <typename T, int N>
bool Tree<T, N>::exists(const T &v) const {
    return _root->exists(v);
}

/** Search the subtree under this node for value \p v.
 * \retval true if the value exists in the subtree under this node. */
template <typename T, int N>
bool Node<T, N>::exists(const T &v) const {
    if (!this)
        return false;

    int i;
    for (i = 0; i < this->n - 1 && v > this->key[i]; ++i)
        /* no-op */;
    if (v == this->key[i])
        return true;
    else if (v < this->key[i])
        return this->child[i]->exists(v);
    else
        return this->child[i+1]->exists(v);
}

/** Return the current height of the tree. */
template <typename T, int N>
int Tree<T, N>::height() const {
    return _root->height();
}

/** \brief Calculate the height of the subtree under this node.
 * \return the calculated height. */
template <typename T, int N>
int Node<T, N>::height() const {
    if (!this) {
        return 0;
    } else {
        int h = 0;
        for (int i = 0; i <= this->n; ++i) {
            int next = this->child[i]->height();
            if (next > h)
                h = next;
        }
        return h + 1;
    }
}

/**
 * \brief Create a new node.
 *
 * Create a new node with one piece of data and children
 * initialized to the given values (may be NULL).
 * @param[in] leftChild the left child of the new node.
 * @param[in] v the value to store in the new node.
 * @param[in] rightChild the right child of the new node.
 */
template <typename T, int N>
Node<T, N>::Node(Node<T, N> *leftChild, const T &v, Node<T, N> *rightChild)
        : n(1)
{
    this->child[0].ptr = leftChild;
    this->key[0] = v;
    this->child[1].ptr = rightChild;
    for (int i = 2; i < childCount; ++i)
        this->child[i].ptr = nullptr;
}

/**
 * Insert a node into the tree
 * @param[in] v the node to insert.
 */
template <typename T, int N>
void Tree<T, N>::insert(T v) {
    node_type *rightNode = nullptr;
    if (!_root->insert(v, rightNode)) {
        // v now contains value that was sent upwards
        _root = new node_type(_root, v, rightNode);
    }
    _root->check_invariants(std::numeric_limits<T>::min(),
                            std::numeric_limits<T>::max());
}

/**
 * \brief Insert an item at a specific node.
 *
 * Insert the item at the given node. If there is room
 * for the value in the node, return \c true. If there is
 * not, return \c false, put a pointer to a new node that
 * was created into \p newNode (containing max value), and
 * put the middle value (to be sent upwards) in \p v.
 * \param[in,out] v on input, the value to insert. On output, the value to
 * push upwards, if this function returned false.
 * \param[out] newNode the node corresponding to the right subtree of \p v.
 * \retval true If the node exists and there is room for the new
 * value.
 * \retval false If this node is \c nullptr or the node is full; in this case \p
 * v and \p newNode will be set to a value and its right subtree respectively
 * which must be inserted into the parent node.
 */
template <typename T, int N>
bool Node<T, N>::insert(T &v, Node<T, N> *&newNode) {
    if (!this) {
        newNode = nullptr;
        return false;
    } else {
        int pos = 0;
        while (pos < this->n && v > this->key[pos])
            pos++;
        if (pos < this->n && v == this->key[pos]) {
            return true;
        } else if (this->child[pos]->insert(v, newNode)) {
            return true;
        } else if (this->n < valueCount) {
            this->add_item(pos, v, newNode);
            return true;
        } else {
            this->split(pos, v, newNode);
            return false;
        }
    }
}

template <typename T, int N>
void Node<T, N>::add_item(int pos, const T &v, Node <T, N> *newNode) {
    assert(pos >= 0 && pos <= this->n);
    for (int i = this->n; i > pos; --i) {
        this->key[i] = this->key[i-1];
        this->child[i+1].ptr = this->child[i].ptr;
    }
    this->key[pos] = v;
    this->child[pos+1].ptr = newNode;
    this->n++;
}

template <typename T, int N>
void Node<T, N>::split(int pos, T &v, Node<T, N> *&newNode) {
    static const int midpoint = (N - 1) / 2;
    assert(this->n == N - 1);
    if (pos > midpoint) {
        /* input value is greater than the value to insert */
        newNode = new Node<T, N>(this->child[midpoint + 1].ptr, v, newNode);
        for (int i = midpoint + 1; i < pos; ++i)
            newNode->add_item(0, this->key[i], this->child[i+1].ptr);
        for (int i = pos; i < this->n; ++i)
            newNode->add_item(i - midpoint, this->key[i], this->child[i+1].ptr);
        std::swap(v, this->key[midpoint]);
    } else if (pos == midpoint) {
        /* input value is the value to insert */
        newNode = new Node<T, N>(newNode, this->key[midpoint],
                                 this->child[midpoint + 1].ptr);
        for (int i = pos + 1; i < this->n; ++i)
            newNode->add_item(i - midpoint, this->key[i], this->child[i+1].ptr);
    } else {
        /* input value is less than the value to insert */
        auto tmp = new Node<T, N>(this->child[midpoint].ptr,
                                  this->key[midpoint],
                                  this->child[midpoint + 1].ptr);
        for (int i = midpoint + 1; i < this->n; ++i)
            tmp->add_item(i - midpoint, this->key[i], this->child[i+1].ptr);
        for (int i = midpoint - 1; i >= pos; --i) {
            this->key[i + 1] = this->key[i];
            this->child[i + 1].ptr = this->child[i].ptr;
        }
        this->key[pos] = v;
        this->child[pos + 1].ptr = newNode;
        v = this->key[midpoint];
        newNode = tmp;
    }
    this->n /= 2;
}

/** \brief Erase node \p v from the tree if it exists.
 * \param[in] v the node to delete.
 */
template <typename T, int N>
void Tree<T, N>::erase(const T &v) {
    Node<T, N>::erase(_root, v);
}

template <typename T, int N>
void Node<T, N>::erase(Node<T, N> *&node, const T &v) {
    if (!node)
        return;

    int pos;
    for (pos = 0; pos < node->n && v > node->key[pos]; ++pos)
        ;
    if (pos < node->n && node->key[pos] == v) {
        if (node->leaf()) {
            node->remove_item(pos);
            if (!node->n) {
                delete node;
                node = nullptr;
            }
        } else if (node->child[pos + 1]->n >= N / 2 + 1) {
            auto *child = node->child[pos + 1].ptr;
            while (!child->leaf()) {
                child = child->child[0].ptr;
            }
            node->key[pos] = child->remove_item(0);
        } else {
            auto *child = node->child[pos].ptr;
            while (!child->leaf()) {
                child = child->child[child->n].ptr;
            }
            node->key[pos] = child->remove_item(child->n - 1);
        }
    }
}

template <typename T, int N>
T Node<T, N>::remove_item(int pos) {
    T result = this->key[pos];
    for (++pos; pos < this->n; ++pos) {
        this->key[pos-1] = this->key[pos];
        this->child[pos-1] = this->child[pos];
    }
    --this->n;
    return result;
}

/** Output to the output stream given by \p out.
 * \param[in] out the stream to output to.
 * \return a reference to the stream.
 */
template <typename T, int N>
std::ostream &Tree<T, N>::output(std::ostream &out) const {
    return _root->output(out, 0);
}

/**
 * Output the node to the given output stream.
 * \param[in] out the stream to output to.
 * \param[in] level the indentation level.
 * \return a reference to the stream.
 */
template <typename T, int N>
std::ostream &Node<T, N>::output(std::ostream &out, int level) const {
    if (this) {
        this->child[0]->output(out, level + 1);
        for (int j = 0; j < this->n; ++j) {
            for (int i = 0; i < level; ++i)
                out << "   ";
            out << this->key[j] << '\n';
            this->child[j+1]->output(out, level + 1);
        }
    }

    return out;
}

/** Check the invariants of this object. */
template <typename T, int N>
void Node<T, N>::check_invariants(T min, const T &max) const {
    if (this) {
        assert(this->n > 0);
        assert(this->n <= valueCount);
        /* Tree is balanced --- all nodes at same depth */
        for (int i = 0; i <= this->n; ++i) {
            if (this->leaf())
                assert(!this->child[i].ptr);
            else {
                assert(this != this->child[i].ptr);
                if (i != 0 && this->child[0]->leaf())
                    assert(this->child[i]->leaf());
            }
        }

        /* Keys are sorted */
        for (int i = 0; i < this->n - 1; ++i) {
            assert(this->key[i] >= min && this->key[i] < this->key[i+1]);
            /* Closure property holds and child node values in correct range */
            this->child[i]->check_invariants(min, this->key[i]);
            min = this->key[i];
        }
        assert(this->key[this->n - 1] <= max);
        this->child[this->n - 1]->check_invariants(min, this->key[this->n - 1]);
        this->child[this->n]->check_invariants(this->key[this->n - 1], max);
    }
}

template class Tree<int, 3>;
template class Tree<int, 4>;
template class Tree<int, 5>;
template class Tree<int, 10>;

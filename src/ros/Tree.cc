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
template <typename T>
Tree<T>::Tree(int degree) : childCount(degree), _root(NULL), version(0) {
}

/** Destroy a tree. */
template <typename T>
Tree<T>::~Tree() {
    delete _root;
}

/** Return true if and only if the tree is empty. */
template <typename T>
bool Tree<T>::isEmpty() const {
    return !_root;
}

/** Destroy the node and its children.  */
template <typename T>
Node<T>::~Node() {
    for (int i = 0; i <= this->n; ++i) {
        delete this->child[i].ptr;
    }
    delete[] this->child;
    delete[] this->key;
}

/** Return true if and only if the value \p v exists in the tree. */
template <typename T>
bool Tree<T>::exists(const T &v) const {
    return _root->exists(v, version);
}

/** Search the subtree under this node for value \p v.
 * \retval true if the value exists in the subtree under this node. */
template <typename T>
bool Node<T>::exists(const T &v, unsigned long version) const {
    if (!this)
        return false;

    int i;
    for (i = 0; i < this->n - 1 && v > this->key[i] || (version < this->child[i].minVersion || version > this->child[i].maxVersion); ++i)
        /* no-op */;
    if (v == this->key[i])
        return true;
    else if (v < this->key[i])
        return this->child[i].ptr->exists(v, version);
    else
        return this->child[i+1].ptr->exists(v, version);
}

/** Return the current height of the tree. */
template <typename T>
int Tree<T>::height() const {
    return _root->height();
}

/** \brief Calculate the height of the subtree under this node.
 * \return the calculated height. */
template <typename T>
int Node<T>::height() const {
    if (!this) {
        return 0;
    } else {
        int h = 0;
        for (int i = 0; i <= this->n; ++i) {
            int next = this->child[i].ptr->height();
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
template <typename T>
Node<T>::Node(Node<T> *leftChild, const T &v, Node<T> *rightChild,
              int maxChildCount)
        : n(1), childCount(maxChildCount), valueCount(maxChildCount - 1)
{
    this->child = new Node::ChildEntry[childCount];
    this->key = new T[valueCount];
    this->child[0].ptr = leftChild;
    this->child[0].minVersion = 0;
    this->child[0].maxVersion = UINT32_MAX;
    this->key[0] = v;
    this->child[1].ptr = rightChild;
    this->child[1].minVersion = 0;
    this->child[1].maxVersion = UINT32_MAX;
    for (int i = 2; i < childCount; ++i) {
        this->child[i].ptr = nullptr;
        this->child[i].minVersion = UINT32_MAX;
        this->child[i].maxVersion = UINT32_MAX;
    }
}

/**
 * Insert a node into the tree
 * @param[in] v the node to insert.
 */
template <typename T>
void Tree<T>::insert(T v) {
    node_type *rightNode = nullptr;
    if (!_root->insert(v, rightNode, version)) {
        // v now contains value that was sent upwards
        _root = new node_type(_root, v, rightNode, childCount);
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
template <typename T>
bool Node<T>::insert(T &v, Node<T> *&newNode, unsigned long version) {
    if (!this) {
        newNode = nullptr;
        return false;
    } else {
        int pos = 0;
        while (pos < this->n && v > this->key[pos])
            pos++;
        if (pos < this->n && v == this->key[pos]) {
            return true;
        } else if (this->child[pos].ptr->insert(v, newNode, version)) {
            return true;
        } else if (this->n < valueCount) {
            this->add_item(pos, v, newNode, version);
            return true;
        } else {
            this->split(pos, v, newNode, version);
            return false;
        }
    }
}

template <typename T>
void Node<T>::add_item(int pos, const T &v, Node<T> *newNode, unsigned long version) {
    assert(pos >= 0 && pos <= this->n);
    for (int i = this->n; i > pos; --i) {
        this->key[i] = this->key[i-1];
	std::swap(this->child[i+1], this->child[i]);
    }
    this->key[pos] = v;
    this->child[pos+1].ptr = newNode;
    this->child[pos+1].minVersion = version;
    this->n++;
}

template <typename T>
void Node<T>::split(int pos, T &v, Node<T> *&newNode, unsigned long version) {
    const int midpoint = (childCount - 1) / 2;
    assert(this->n == childCount - 1);
    if (pos > midpoint) {
        /* input value is greater than the value to insert */
        newNode = new Node<T>(this->child[midpoint + 1].ptr, v, newNode,
			      childCount);
        for (int i = midpoint + 1; i < pos; ++i)
            newNode->add_item(0, this->key[i], this->child[i+1].ptr, version);
        for (int i = pos; i < this->n; ++i)
            newNode->add_item(i - midpoint, this->key[i], this->child[i+1].ptr, version);
        std::swap(v, this->key[midpoint]);
    } else if (pos == midpoint) {
        /* input value is the value to insert */
        newNode = new Node<T>(newNode, this->key[midpoint],
                                 this->child[midpoint + 1].ptr, childCount);
        for (int i = pos + 1; i < this->n; ++i)
            newNode->add_item(i - midpoint, this->key[i], this->child[i+1].ptr, version);
    } else {
        /* input value is less than the value to insert */
        auto tmp = new Node<T>(this->child[midpoint].ptr,
                                  this->key[midpoint],
                                  this->child[midpoint + 1].ptr, childCount);
        for (int i = midpoint + 1; i < this->n; ++i)
            tmp->add_item(i - midpoint, this->key[i], this->child[i+1].ptr, version);
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
template <typename T>
void Tree<T>::erase(const T &v) {
    Node<T>::erase(_root, v, version);
}

template <typename T>
void Node<T>::erase(Node<T> *&node, const T &v, unsigned long version) {
    if (!node)
        return;

    int pos;
    for (pos = 0; pos < node->n && v > node->key[pos]; ++pos)
        ;
    if (pos < node->n && node->key[pos] == v) {
        if (node->leaf()) {
            node->remove_item(pos, version);
            if (!node->n) {
                delete node;
                node = nullptr;
            }
        } else if (node->child[pos + 1].ptr->n >= node->childCount / 2 + 1) {
            Node *child = node->child[pos + 1].ptr;
            while (!child->leaf()) {
                child = child->child[0].ptr;
            }
            node->key[pos] = child->remove_item(0, version);
        } else {
            Node *child = node->child[pos].ptr;
            while (!child->leaf()) {
                child = child->child[child->n].ptr;
            }
            node->key[pos] = child->remove_item(child->n - 1, version);
        }
    }
}

template <typename T>
T Node<T>::remove_item(int pos, unsigned long version) {
    T result = this->key[pos];
    this->child[pos].maxVersion = version;
    return result;
}

/** Output to the output stream given by \p out.
 * \param[in] out the stream to output to.
 * \return a reference to the stream.
 */
template <typename T>
std::ostream &Tree<T>::output(std::ostream &out) const {
    return _root->output(out, 0, version);
}

/**
 * Output the node to the given output stream.
 * \param[in] out the stream to output to.
 * \param[in] level the indentation level.
 * \return a reference to the stream.
 */
template <typename T>
std::ostream &Node<T>::output(std::ostream &out, int level, unsigned long version) const {
    if (this) {
        if (version >= this->child[0].minVersion && version < this->child[0].maxVersion)
            this->child[0].ptr->output(out, level + 1, version);
        for (int j = 0; j < this->n; ++j) {
            for (int i = 0; i < level; ++i)
                out << "   ";
            out << this->key[j] << '\n';
            if (version >= this->child[j+1].minVersion && version < this->child[j+1].maxVersion)
                this->child[j+1].ptr->output(out, level + 1, version);
        }
    }

    return out;
}

/** Check the invariants of this object. */
template <typename T>
void Node<T>::check_invariants(T min, const T &max) const {
    if (this) {
        assert(this->n > 0);
        assert(this->n <= valueCount);
        /* Tree is balanced --- all nodes at same depth */
        for (int i = 0; i <= this->n; ++i) {
            if (this->leaf())
                assert(!this->child[i].ptr);
            else {
                assert(this != this->child[i].ptr);
                if (i != 0 && this->child[0].ptr->leaf())
                    assert(this->child[i].ptr->leaf());
            }
        }

        /* Keys are sorted */
        for (int i = 0; i < this->n - 1; ++i) {
            assert(this->key[i] >= min && this->key[i] < this->key[i+1]);
            /* Closure property holds and child node values in correct range */
            this->child[i].ptr->check_invariants(min, this->key[i]);
            min = this->key[i];
        }
        assert(this->key[this->n - 1] <= max);
        this->child[this->n - 1].ptr->check_invariants(min, this->key[this->n - 1]);
        this->child[this->n].ptr->check_invariants(this->key[this->n - 1], max);
    }
}

#if 0
/**
 * Allocate storage for a node. */
template <typename T>
void *Node<T>::operator new(size_t size)
{
}

/**
 * Frees storage for a node allocated with operator new overload. */
template <typename T>
void Node<T>operator delete(void *p)
{
	free(p);
}
#endif

template class Tree<int>;
template class Node<int>;

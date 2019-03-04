#ifndef TREE_H
#define TREE_H

/** \file Tree.h
 * \author Patrick MacArthur
 *
 * Declare B-Tree class.
 */

#include <cstdint>
#include <iosfwd>

template <typename T>
class Node;

/** Represent a B-tree. */
template <typename T>
class Tree {
public:
    Tree(int degree);
    ~Tree();
    Tree(Tree<T> &) = delete;
    Tree &operator=(Tree<T> &) = delete;
    void insert(T v);
    void erase(const T &v);
    bool exists(const T &) const;
    int height() const;
    std::ostream &output(std::ostream &) const;
    bool isEmpty() const;

private:
    int childCount;
    using node_type = ::Node<T>;
    using value_type = T;
    node_type *_root;
    uint32_t version;
};

extern template class Tree<int>;

/** Output the structural representation of a B-tree to stream \p s. */
template <typename T>
inline std::ostream &operator<<(std::ostream &s, const Tree<T> &t) {
    return t.output(s);
}

#endif /* not TREE_H */

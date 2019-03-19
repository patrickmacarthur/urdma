#ifndef TREE_H
#define TREE_H

/** \file Tree.h
 * \author Patrick MacArthur
 *
 * Declare B-Tree class.
 */

#include <iosfwd>

template <typename T, int N>
class Node;

/** Represent a B-tree. */
template <typename T, int N>
class Tree {
public:
    Tree();
    ~Tree();
    Tree(Tree<T, N> &) = delete;
    Tree &operator=(Tree<T, N> &) = delete;
    void insert(T v);
    void erase(const T &v);
    bool exists(const T &) const;
    int height() const;
    std::ostream &output(std::ostream &) const;
    bool isEmpty() const;

private:
    friend class TwoThreeTreeFullInternalTest;
    friend class BTreeFiveFullInternalTest;
    static const int childCount = N;
    using node_type = ::Node<T, childCount>;
    using value_type = T;
    node_type *_root;
};

extern template class Tree<int, 3>;
extern template class Tree<int, 4>;
extern template class Tree<int, 5>;
extern template class Tree<int, 10>;

/** \brief Represents a 2-3 tree.
 *
 * This template specializes the B-tree of degree 3, also known as a 2-3 tree.
 */
template <typename T>
using TwoThreeTree = Tree<T, 3>;

/** \brief Represents a 2-3-4 tree.
 *
 * This template specializes the B-tree of degree 4, also known as a 2-3-4
 * tree.
 */
template <typename T>
using TwoThreeFourTree = Tree<T, 4>;

/** Output the structural representation of a B-tree to stream \p s. */
template <typename T, int N>
inline std::ostream &operator<<(std::ostream &s, const Tree<T, N> &t) {
    return t.output(s);
}

#endif /* not TREE_H */

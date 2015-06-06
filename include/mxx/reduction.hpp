/**
 * @file    reduction.hpp
 * @author  Patrick Flick <patrick.flick@gmail.com>
 * @brief   Reduction operations.
 *
 * TODO add Licence
 */


#ifndef MXX_REDUCTION_HPP
#define MXX_REDUCTION_HPP

#include <mpi.h>

#include <vector>
#include <iterator>
#include <limits>
#include <functional>
#include <mutex>
#include <atomic>

// mxx includes
#include "datatypes.hpp"
#include "comm.hpp"

namespace mxx {

/*
 * Add any (key,value) pair to any MPI_Datatype using MPI caching (keyval)
 * functionality (used internally to mxx for adding std::function objects to
 * MPI datatypes, required for custom operators)
 */
template <typename K, typename T>
class attr_map {
private:
    // static "variables" wrapped into static member functions
    static int& key(){ static int k=0; return k; }
    static std::mutex& mut(){ static std::mutex m; return m; }
    static std::map<K, int>& keymap(){ static std::map<K, int> m; return m; }

    static int copy_attr(MPI_Datatype, int, void*, void *attribute_val_in, void *attribute_val_out, int *flag) {
        T** out = (T**) attribute_val_out;
        // copy via copy constructor
        *out = new T(*(T*)(attribute_val_in));
        *flag = 1;
        return MPI_SUCCESS;
    }

    static int del_attr(MPI_Datatype, int, void *attribute_val, void *) {
        delete (T*)attribute_val;
        return MPI_SUCCESS;
    }
public:
    static void set(const MPI_Datatype& dt, const K& k, const T& value) {
        std::lock_guard<std::mutex> lock(mut());
        if (keymap().find(k) == keymap().end()) {
            // create new keyval with MPI
            keymap()[k] = 0;
            MPI_Type_create_keyval(&attr_map<K,T>::copy_attr,
                                   &attr_map<K,T>::del_attr,
                                   &(keymap()[k]), (void*)NULL);
        }
        // insert new key into keymap
        // set the keyval pair
        T* val = new T(value); // copy construct
        MPI_Type_set_attr(dt, keymap()[k], (void*)val);
    }
    static T& get(const MPI_Datatype& dt, const K& k) {
        std::lock_guard<std::mutex> lock(mut());
        if (keymap().find(k) == keymap().end()) {
            throw std::out_of_range("Key not mapped.");
        }
        int key = keymap()[k];
        T* result;
        int flag;
        MPI_Type_get_attr(dt, key, &result, &flag);
        if (!flag) {
            throw std::out_of_range("Key not mapped.");
        }
        return *result;
    }
};

/*********************************************************************
 *                     User supplied functions:                      *
 *********************************************************************/

// an Op is not built-in in general:
template <typename T, typename Func>
struct get_builtin_op {
    static MPI_Op op(Func) {
        return MPI_OP_NULL;
    }
};

// define all C++ functors that map to a MPI builtin MPI_Op to directly map
// to that builtin MPI_Op
#define MXX_BUILTIN_OP(mpi_op, cpp_functor)                                    \
template <typename T>                                                          \
struct get_builtin_op<T, cpp_functor<T> > {                                      \
    static MPI_Op op(cpp_functor<T>) {                                           \
        return mpi_op;                                                        \
    }                                                                          \
};                                                                             \

MXX_BUILTIN_OP(MPI_SUM, std::plus);
MXX_BUILTIN_OP(MPI_PROD, std::multiplies);
MXX_BUILTIN_OP(MPI_LAND, std::logical_and);
MXX_BUILTIN_OP(MPI_LOR, std::logical_or);
MXX_BUILTIN_OP(MPI_BOR, std::bit_or);
MXX_BUILTIN_OP(MPI_BXOR, std::bit_xor);
MXX_BUILTIN_OP(MPI_BAND, std::bit_and);

#undef MXX_BUILTIN_OP


// for std::min/std::max functions
// TODO: this doesn't find the type if it is passed as Func& func in custom_op constructor
template <typename T>
struct get_builtin_op<T, const T&(*) (const T&, const T&)> {
    static MPI_Op op(const T& (*t)(const T&, const T&)) {
        // check if function is std::min or std::max
        if (t == static_cast<const T&(*)(const T&, const T&)>(std::min<T>)){
            return MPI_MIN;
        } else if (t == static_cast<const T&(*)(const T&, const T&)>(std::max<T>)){
            return MPI_MAX;
        } else {
            // otherwise return NULL
            return MPI_OP_NULL;
        }
    }
};


/**
 * @brief   Wrapps a binary combination/reduction operator for MPI use in
 *          custom operators.
 *
 * @note    This assumes that the operator is commutative.
 *
 * @tparam T    The input and ouput datatype of the binary operator.
 * @tparam IsCommutative    Whether or not the operation is commutative (default = true).
 */
template <typename T, bool IsCommutative = true>
class custom_op {
public:

    /**
     * @brief Creates a custom operator given a functor and the associated
     *        `MPI_Datatype`.
     *
     * @tparam Func     Type of the functor, can be a function pointer, lambda
     *                  function, or std::function or any object with a
     *                  `T operator(T& x, T& y)` member.
     * @param func      The instance of the functor.
     */
    template <typename Func>
    custom_op(Func& func) : builtin(false) {
        if (mxx::is_builtin_type<T>::value) {
            // check if the operator is MPI built-in (in case the type
            // is also a MPI built-in type)
            // TODO: somehow having `func` as a reference parameter
            //       vs non reference parameter messes up different
            //       test cases. what's going on??
            // TODO: quetsion is: how are function pointers passed when
            //       the input parameter is a reference of templated type???
            MPI_Op o = get_builtin_op<T, Func>::op(func);
            if (o != MPI_OP_NULL) {
                // this op is builtin, save it as such and don't copy built-in type
                builtin = true;
                op = o;
                mxx::datatype<T> dt;
                type_copy = dt.type();
            }
        }
        if (!builtin) {
            // create user function
            typedef std::function<void(void*,void*,int*)> func_t;
            func_t user_func = [&func](void* invec, void* inoutvec, int* len) {
                T* in = (T*) invec;
                T* inout = (T*) inoutvec;
                for (int i = 0; i < *len; ++i) {
                    std::cout << "i=" << i << ", len = " << *len << ", in[i]=" << in[i] << ", inout[i]=" << inout[i] << std::endl;
                    std::cout << "&f=" << &func << std::endl;
                    //std::cout << "f=" << func << std::endl;
                    inout[i] = func(in[i], inout[i]);
                }
            };
            // get datatype associated with the type `T`
            mxx::datatype<T> dt;
            // attach function to a copy of the datatype
            MPI_Type_dup(dt.type(), &type_copy);
            attr_map<int, func_t>::set(type_copy, 1347, user_func);
            // create op
            MPI_Op_create(&custom_op::user_function, IsCommutative, &op);
        }
    }

    /**
     * @brief   Returns the MPI_Datatype which has to be used in conjuction
     *          with the MPI_Op operator.
     *
     * The custom operators are wrapped into `std::function` objects and
     * saved/attached to a duplicated MPI_Datatype as MPI attribute.
     *
     * When MPI calls the custom user function, the MPI_Datatype is supplied
     * and thus the `std::function` object can be accessed and executed.
     *
     * @returns The `MPI_Datatype` which has to be used in conjuction with the
     *          `MPI_Op` returned by `get_op()` for all MPI reduction operations.
     */
    MPI_Datatype get_type() const {
        return type_copy;
    }

    /**
     * @brief   Returns the `MPI_Op` operator for reduction operations.
     *
     * @note
     * The MPI operator `MPI_Op` returned by this function can only be used in
     * conjunction with the `MPI_Datatype` returned by `get_type()`.
     *
     * @returns     The MPI operator as `MPI_Op` object.
     */
    MPI_Op get_op() const {
        return op;
    }

    /// Destructor: cleanup MPI objects
    virtual ~custom_op() {
        if (!builtin) {
            // clean-up (only if this wasn't a built-in MPI_Op)
            MPI_Op_free(&op);
            MPI_Type_free(&type_copy);
        }
    }
private:
    // MPI custom Op function: (of type MPI_User_function)
    static void user_function(void* in, void* inout, int* n, MPI_Datatype* dt) {
        // get the std::function from the MPI_Datatype and call it
        typedef std::function<void(void*,void*,int*)> func_t;
        func_t f = attr_map<int, func_t>::get(*dt, 1347);
        f(in, inout, n);
    }

    /// Whether the MPI_Op is a builtin operator (e.g. MPI_SUM)
    bool builtin;
    /// The copy (Type_dup) of the MPI_Datatype to work on
    MPI_Datatype type_copy;
    /// The MPI user operator
    MPI_Op op;
};


/*********************************************************************
 *                Reductions                                         *
 *********************************************************************/
// TODO: add more (vectorized (std::vector, [begin,end),...), different reduce ops, etc)
// TODO: naming of functions !?
// TODO: come up with good naming scheme and standardize!
// TODO: template specialize for std::min, std::max, std::plus, std::multiply
//       etc for integers to use MPI builtin ops?


template <typename T, typename Func>
T reduce(const T& x, int root, Func func, const mxx::comm& comm = mxx::comm()) {
    // get custom op (and type for custom op)
    mxx::custom_op<T> op(func);
    T result = T();
    MPI_Reduce(const_cast<T*>(&x), &result, 1, op.get_type(), op.get_op(), root, comm);
    return result;
}

template <typename T>
T reduce(const T& x, int root, const mxx::comm& comm = mxx::comm()) {
    return reduce(x, root, std::plus<T>(), comm);
}

// TODO: vectorized ops

template <typename T, typename Func>
T allreduce(const T& x, Func func, const mxx::comm& comm = mxx::comm()) {
    // get custom op (and type for custom op)
    mxx::custom_op<T> op(func);
    // perform reduction
    T result;
    MPI_Allreduce(const_cast<T*>(&x), &result, 1, op.get_type(), op.get_op(), comm);
    return result;
}

template <typename T>
T allreduce(const T& x, const mxx::comm& comm = mxx::comm()) {
    return allreduce(x, std::plus<T>(), comm);
}

template <typename T, typename Func>
T scan(const T& x, Func func, const mxx::comm& comm = mxx::comm()) {
    // get op
    mxx::custom_op<T> op(func);
    T result;
    MPI_Scan(const_cast<T*>(&x), &result, 1, op.get_type(), op.get_op(), comm);
    return result;
}
template <typename T>
T scan(const T& x, const mxx::comm& comm = mxx::comm()) {
    return scan(x, std::plus<T>(), comm);
}


template <typename T, typename Func>
T exscan(const T& x, Func func, const mxx::comm& comm = mxx::comm()) {
    // get op
    mxx::custom_op<T> op(func);
    // perform reduction
    T result;
    MPI_Exscan(const_cast<T*>(&x), &result, 1, op.get_type(), op.get_op(), comm);
    if (comm.rank() == 0)
      result = T();
    return result;
}

template <typename T>
T exscan(const T& x, const mxx::comm& comm = mxx::comm()) {
    return exscan(x, std::plus<T>(), comm);
}

/****************************************************
 *  reverse reductions (with reverse communicator)  *
 ****************************************************/

inline void rev_comm(MPI_Comm comm, MPI_Comm& rev)
{
    // get MPI parameters
    int rank;
    int p;
    MPI_Comm_rank(comm, &rank);
    MPI_Comm_size(comm, &p);
    MPI_Comm_split(comm, 0, p - rank, &rev);
}

template <typename T>
T reverse_exscan(T& x, const mxx::comm& comm = mxx::comm()) {
    MPI_Comm rev;
    rev_comm(comm, rev);
    T result = exscan(x, rev);
    MPI_Comm_free(&rev);
    return result;
}

template <typename T, typename Func>
T reverse_exscan(T& x, Func func, const mxx::comm& comm = mxx::comm()) {
    MPI_Comm rev;
    rev_comm(comm, rev);
    T result = exscan(x, func, rev);
    MPI_Comm_free(&rev);
    return result;
}

template <typename T>
T reverse_scan(T& x, const mxx::comm& comm = mxx::comm()) {
    MPI_Comm rev;
    rev_comm(comm, rev);
    T result = scan(x, rev);
    MPI_Comm_free(&rev);
    return result;
}

template <typename T, typename Func>
T reverse_scan(T& x, Func func, const mxx::comm& comm = mxx::comm()) {
    MPI_Comm rev;
    rev_comm(comm, rev);
    T result = scan(x, func, rev);
    MPI_Comm_free(&rev);
    return result;
}


/*********************
 *  Specialized ops  *
 *********************/

/************************
 *  Boolean reductions  *
 ************************/
// useful for testing global conditions, such as termination conditions

inline bool all_of(bool x, const mxx::comm& comm = mxx::comm()) {
    int i = x ? 1 : 0;
    int result;
    MPI_Allreduce(&i, &result, 1, MPI_INT, MPI_LAND, comm);
    return result != 0;
}

inline bool any_of(bool x, const mxx::comm& comm = mxx::comm()) {
    int i = x ? 1 : 0;
    int result;
    MPI_Allreduce(&i, &result, 1, MPI_INT, MPI_LOR, comm);
    return result != 0;
}

inline bool none_of(bool x, const mxx::comm& comm = mxx::comm()) {
    int i = x ? 1 : 0;
    int result;
    MPI_Allreduce(&i, &result, 1, MPI_INT, MPI_LAND, comm);
    return result == 0;
}

} // namespace mxx

#endif // MXX_REDUCTION_HPP
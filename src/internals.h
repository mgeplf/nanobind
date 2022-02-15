#include <nanobind/nanobind.h>
#include <tsl/robin_map.h>
#include <tsl/robin_set.h>
#include <typeindex>

NAMESPACE_BEGIN(NB_NAMESPACE)
NAMESPACE_BEGIN(detail)

/// Nanobind function metadata (signature, overloads, etc.)
struct func_record : func_data<0> {
    arg_data *args;

    /// Function signature in string format
    char *signature;
};

/// Python object representing an instance of a bound C++ type
struct nb_inst {
    PyObject_HEAD
    void *value;
    /// Should the destructor be called when this instance is GCed?
    bool destruct : 1;

    /// Should the instance pointer be freed when this instance is GCed?
    bool free : 1;

    /// Does this instance hold reference to others? (via instance->refs)
    bool clear_keep_alive : 1;
};

/// Python object representing a bound C++ function
struct nb_func {
    PyObject_VAR_HEAD
    PyObject* (*vectorcall)(PyObject *, PyObject * const*, size_t, PyObject *);
    uint32_t max_nargs_pos;
    bool is_complex;
};

struct ptr_hash {
    NB_INLINE size_t operator()(const std::pair<void *, std::type_index> &value) const {
        size_t hash_1 = (size_t) value.first;
        hash_1 = (hash_1 >> 4) | (hash_1 << (8 * sizeof(size_t) - 4));
        size_t hash_2 = value.second.hash_code();
        return hash_1 + hash_2 * 3;
    }
};

struct internals {
    /// Base type of all nanobind types
    PyTypeObject *nb_type;

    /// Base type of all nanobind functions
    PyTypeObject *nb_func;

    /// Base type of all nanobind methods
    PyTypeObject *nb_meth;

    /// Instance pointer -> Python object mapping
    tsl::robin_pg_map<std::pair<void *, std::type_index>, nb_inst *, ptr_hash> inst_c2p;

    /// C++ type -> Python type mapping
    tsl::robin_pg_map<std::type_index, type_data *> type_c2p;

    /// Python dictionary of sets storing keep_alive references
    PyObject *keep_alive;

    /// Python set of functions for docstring generation
    PyObject *funcs;

    std::vector<void (*)(std::exception_ptr)> exception_translators;
};

extern internals &get_internals() noexcept;

/* The following two functions convert between pointers and Python long values
   that can be used as hash keys. They internally perform rotations to avoid
   collisions following 'pyhash.c' */
inline PyObject *ptr_to_key(void *p) {
    uintptr_t i = (uintptr_t) p;
    i = (i >> 4) | (i << (8 * sizeof(uintptr_t) - 4));
    return PyLong_FromSsize_t((Py_ssize_t) i);
}

inline void *key_to_ptr(PyObject *o) {
    uintptr_t i = (uintptr_t) PyLong_AsSsize_t(o);
    i = (i << 4) | (i >> (8 * sizeof(uintptr_t) - 4));
    return (void *) i;
}

NAMESPACE_END(detail)
NAMESPACE_END(NB_NAMESPACE)

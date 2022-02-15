#include "internals.h"

NAMESPACE_BEGIN(NB_NAMESPACE)
NAMESPACE_BEGIN(detail)

// PyType_GenericAlloc alternative that allocates extra space at the end
static PyObject *alloc_extra(PyTypeObject *type, size_t extra) {
    size_t item_size  = (size_t) type->tp_itemsize,
           item_count = (extra + item_size - 1) / item_size;

    /* PyType_GenericAlloc reserves space for a sentinel element that we do not
       require. It is intentional that the argument to PyType_GenericAlloc may
       then be come negative.) */
    PyVarObject *o =
        (PyVarObject *) PyType_GenericAlloc(type, (Py_ssize_t) item_count - 1);
    if (!o)
        return nullptr;
    o->ob_size = 0;
    return (PyObject *) o;
}

template <typename T>
NB_INLINE T *get_extra(void *o) {
    return (T*) ((char *) o + Py_TYPE(o)->tp_basicsize);
}

static int inst_init(PyObject *self, PyObject *, PyObject *) {
    PyErr_Format(PyExc_TypeError, "%s: no constructor defined!",
                 Py_TYPE(self)->tp_name);
    return -1;
}

// Allocate a new instance with co-located or external storage
nb_inst *inst_new_impl(PyTypeObject *type, void *value) {
    PyVarObject *o = (PyVarObject *) PyType_GenericAlloc(type, value ? -1 : 0);
    o->ob_size = 0;

    nb_inst *self = (nb_inst *) o;
    const type_data *t = get_extra<type_data>(type);
    if (value) {
        self->value = value;
    } else {
        // Re-align address
        uintptr_t align = t->align,
                  offset = (uintptr_t) get_extra<void>(self);

        offset = (offset + align - 1) / align * align;
        self->value = (void *) offset;
    }

    // Update hash table that maps from C++ to Python instance
    auto [it, success] = get_internals().inst_c2p.try_emplace(
        std::pair<void *, std::type_index>(self->value, *t->type), self);

    if (!success)
        fail("nanobind::detail::inst_new(): duplicate object!");

    return self;
}

// Allocate a new instance with co-located storage
PyObject *inst_new(PyTypeObject *type, PyObject *, PyObject *) {
    return (PyObject *) inst_new_impl(type, nullptr);
}

static void inst_dealloc(PyObject *self) {
    nb_inst *inst = (nb_inst *) self;
    PyTypeObject *type = Py_TYPE(self);

    type_data *t = get_extra<type_data>(type);

    if (inst->destruct) {
        if (t->flags & (int16_t) type_flags::is_destructible) {
            if (t->flags & (int16_t) type_flags::has_destruct)
                t->destruct(inst->value);
        } else {
            fail("nanobind::detail::inst_dealloc(\"%s\"): attempted to call "
                 "the destructor of a non-destructible type!", type->tp_name);
        }
    }

    if (inst->free) {
        if (t->align <= __STDCPP_DEFAULT_NEW_ALIGNMENT__)
            operator delete(inst->value);
        else
            operator delete(inst->value, std::align_val_t(t->align));
    }

    internals &internals = get_internals();
    if (inst->clear_keep_alive) {
        PyObject *self_key = ptr_to_key(self),
                 *set = PyDict_GetItem(internals.keep_alive, self_key);

        int rv = PyDict_DelItem(internals.keep_alive, self_key);
        if (rv || set == nullptr)
            fail("nanobind::detail::inst_dealloc(\"%s\"): failure while "
                 "clearing references!", type->tp_name);

        Py_ssize_t i = 0;
        PyObject *key;
        Py_hash_t hash;

        while (_PySet_NextEntry(set, &i, &key, &hash))
            Py_DECREF((PyObject *) key_to_ptr(key));

        Py_DECREF(set);
        Py_DECREF(self_key);
    }

    // Update hash table that maps from C++ to Python instance
    auto it = internals.inst_c2p.find(
        std::pair<void *, std::type_index>(inst->value, *t->type));
    if (it == internals.inst_c2p.end())
        fail("nanobind::detail::inst_dealloc(\"%s\"): attempted to delete "
             "an unknown instance!", type->tp_name);
    internals.inst_c2p.erase(it);

    type->tp_free(self);
    Py_DECREF(type);
}

void type_free(PyObject *o) {
    PyTypeObject *tp = (PyTypeObject *) o;
    type_data *t = get_extra<type_data>(tp);

    // Try to find type in data structure
    internals &internals = get_internals();
    auto it = internals.type_c2p.find(std::type_index(*t->type));
    if (it == internals.type_c2p.end())
        fail("nanobind::detail::type_free(\"%s\"): could not find type!",
             ((PyTypeObject *) o)->tp_name);
    internals.type_c2p.erase(it);

    // Free Python type object
    PyType_Type.tp_dealloc(o);
}

PyObject *type_new(const type_data *t) noexcept {
    const bool has_scope   = t->flags & (uint16_t) type_flags::has_scope,
               has_doc     = t->flags & (uint16_t) type_flags::has_doc,
               has_base    = t->flags & (uint16_t) type_flags::has_base,
               has_base_py = t->flags & (uint16_t) type_flags::has_base_py;

    if (has_base && has_base_py)
        fail("nanobind::detail::type_new(\"%s\"): multiple base types "
             "specified!", t->name);

    str name(t->name), qualname = name, fullname = name;

    if (has_scope && !PyModule_Check(t->scope)) {
        object scope_qualname = borrow(getattr(t->scope, "__qualname__", nullptr));
        if (scope_qualname.is_valid())
            qualname = steal<str>(PyUnicode_FromFormat(
                "%U.%U", scope_qualname.ptr(), name.ptr()));
    }

    object scope_name;
    if (has_scope) {
        scope_name = getattr(t->scope, "__module__", handle());
        if (!scope_name.is_valid())
            scope_name = getattr(t->scope, "__name__", handle());

        if (scope_name.is_valid())
            fullname = steal<str>(
                PyUnicode_FromFormat("%U.%U", scope_name.ptr(), name.ptr()));
    }

    /* Danger zone: from now (and until PyType_Ready), make sure to
       issue no Python C API calls which could potentially invoke the
       garbage collector (the GC will call type_traverse(), which will in
       turn find the newly constructed type in an invalid state) */

    internals &internals = get_internals();
    PyHeapTypeObject *ht = (PyHeapTypeObject *) alloc_extra(internals.nb_type,
                                                            sizeof(type_data));
    type_data *t2 = get_extra<type_data>(ht);
    memcpy(t2, t, sizeof(type_data));

    ht->ht_name = name.release().ptr();
    ht->ht_qualname = qualname.release().ptr();

    PyTypeObject *type = &ht->ht_type;

    type->tp_name = t->name;
    if (has_doc)
        type->tp_doc = t->doc;

    type->tp_basicsize = (Py_ssize_t) sizeof(nb_inst);
    type->tp_itemsize = (Py_ssize_t) t->size;

    // Potentially insert extra space for alignment
    if (t->align > sizeof(void *))
        type->tp_itemsize += (Py_ssize_t) (t->align - sizeof(void *));

    type->tp_init = inst_init;
    type->tp_new = inst_new;
    type->tp_dealloc = inst_dealloc;
    type->tp_as_number = &ht->as_number;
    type->tp_as_sequence = &ht->as_sequence;
    type->tp_as_mapping = &ht->as_mapping;
    type->tp_flags |= Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_BASETYPE;

    if (PyType_Ready(type) < 0)
        fail("nanobind::detail::type_new(\"%s\"): PyType_Ready() failed!", t->name);

    if (scope_name.is_valid())
        setattr((PyObject *) type, "__module__", scope_name);

    if (has_scope)
        setattr(t->scope, t->name, (PyObject *) type);

    t2->type_py = type;

    // Update hash table that maps from std::type_info to Python type
    auto [it, success] =
        internals.type_c2p.try_emplace(std::type_index(*t->type), t2);
    if (!success)
        fail("nanobind::detail::type_new(\"%s\"): type was already registered!",
             t->name);

    return (PyObject *) type;
}

bool type_get(const std::type_info *cpp_type, PyObject *o, bool ,
              void **out) noexcept {
    if (o == nullptr || o == Py_None) {
        *out = nullptr;
        return o != nullptr;
    }

    internals &internals = get_internals();
    PyTypeObject *type = Py_TYPE(o);

    // Reject if this object doesn't have the nanobind metaclass
    if (Py_TYPE(type) != internals.nb_type)
        return false;

    // Recover pointer to C++ type_data entry
    type_data *t = get_extra<type_data>(type);

    // Fast path
    if (t->type == cpp_type || *t->type == *cpp_type) {
        *out = ((nb_inst *) o)->value;
        return true;
    } else {
        return false;
    }
}

void inst_keep_alive(PyObject *nurse, PyObject *patient) {
    if (!patient)
        return;

    internals &internals = get_internals();
    if (!nurse || Py_TYPE(Py_TYPE(nurse)) != internals.nb_type)
        raise("inst_keep_alive(): expected a nb_type 'nurse' argument");

    PyObject *nurse_key   = ptr_to_key(nurse),
             *patient_key = ptr_to_key(patient);

    PyObject *nurse_set = PyDict_GetItem(internals.keep_alive, nurse_key);
    int rv;

    if (!nurse_set) {
        PyErr_Clear();
        nurse_set = PySet_New(nullptr);
        if (!nurse_set)
            goto error;

        rv = PyDict_SetItem(internals.keep_alive, nurse_key, nurse_set);
        if (rv)
            goto error;
    }

    rv = PySet_Contains(nurse_set, patient_key);
    if (rv == 0) {
        int rv = PySet_Add(nurse_set, patient_key);
        if (rv)
            goto error;

        Py_INCREF(patient);
        ((nb_inst *) nurse)->clear_keep_alive = true;
    } else if (rv < 0) {
        goto error;
    }

    Py_DECREF(nurse_key);
    Py_DECREF(patient_key);
    Py_DECREF(nurse_set);

    return;

error:
    fail("nanobind::detail::inst_keep_alive(): internal error!");
}

PyObject *type_put(const std::type_info *cpp_type, void *value,
                   rv_policy rvp, PyObject *parent) noexcept {
    // Convert nullptr -> None
    if (!value) {
        Py_INCREF(Py_None);
        return Py_None;
    }

    // Check if the nb_inst is already registered with nanobind
    internals &internals = get_internals();
    auto it = internals.inst_c2p.find(
        std::pair<void *, std::type_index>(value, *cpp_type));
    if (it != internals.inst_c2p.end()) {
        PyObject *result = (PyObject *) it->second;
        Py_INCREF(result);
        return result;
    } else if (rvp == rv_policy::none) {
        return nullptr;
    }

    // Look up the corresponding type
    auto it2 = internals.type_c2p.find(std::type_index(*cpp_type));
    if (it2 == internals.type_c2p.end())
        return nullptr;

    type_data *t = it2->second;

    bool store_in_obj = rvp == rv_policy::copy || rvp == rv_policy::move;

    nb_inst *inst = inst_new_impl(t->type_py, store_in_obj ? nullptr : value);
    inst->destruct =
        rvp != rv_policy::reference && rvp != rv_policy::reference_internal;
    inst->free = inst->destruct && !store_in_obj;

    if (rvp == rv_policy::reference_internal)
        inst_keep_alive((PyObject *) inst, parent);

    if (rvp == rv_policy::move) {
        if (t->flags & (uint16_t) type_flags::is_move_constructible) {
            if (t->flags & (uint16_t) type_flags::has_move) {
                try {
                    t->move(inst->value, value);
                } catch (...) {
                    Py_DECREF(inst);
                    return nullptr;
                }
            } else {
                memcpy(inst->value, value, t->size);
            }
        } else {
            fail("nanobind::detail::type_put(\"%s\"): attempted to move "
                 "an instance that is not move-constructible!", t->name);
        }
    }

    if (rvp == rv_policy::copy) {
        if (t->flags & (uint16_t) type_flags::is_copy_constructible) {
            if (t->flags & (uint16_t) type_flags::has_copy) {
                try {
                    t->copy(inst->value, value);
                } catch (...) {
                    Py_DECREF(inst);
                    return nullptr;
                }
            } else {
                memcpy(inst->value, value, t->size);
            }
        } else {
            fail("nanobind::detail::type_put(\"%s\"): attempted to copy "
                 "an instance that is not copy-constructible!", t->name);
        }
    }

    return (PyObject *) inst;
}

NAMESPACE_END(detail)
NAMESPACE_END(NB_NAMESPACE)
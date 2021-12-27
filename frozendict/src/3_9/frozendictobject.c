#include <Python.h>
#include "frozendictobject.h"
#include "dictobject.c"
#include "dict-common.h"

static Py_ssize_t
lookdict_unicode_nodummy(PyDictObject *mp, PyObject *key,
                         Py_hash_t hash, PyObject **value_addr);
static PyObject* frozendict_iter(PyDictObject *dict);

static void fd_free_keys_object(PyDictKeysObject *keys, const int decref_items);

static inline void
fd_dictkeys_decref(PyDictKeysObject *dk, const int decref_items)
{
    assert(dk->dk_refcnt > 0);
#ifdef Py_REF_DEBUG
    _Py_RefTotal--;
#endif
    if (--dk->dk_refcnt == 0) {
        fd_free_keys_object(dk, decref_items);
    }
}

/* Find the smallest dk_size >= minsize. */
static inline Py_ssize_t
calculate_keysize(Py_ssize_t minsize)
{
#if SIZEOF_LONG == SIZEOF_SIZE_T
    minsize = (minsize | PyDict_MINSIZE) - 1;
    return 1LL << _Py_bit_length(minsize | (PyDict_MINSIZE-1));
#elif defined(_MSC_VER)
    // On 64bit Windows, sizeof(long) == 4.
    minsize = (minsize | PyDict_MINSIZE) - 1;
    unsigned long msb;
    _BitScanReverse64(&msb, (uint64_t)minsize);
    return 1LL << (msb + 1);
#else
    Py_ssize_t size;
    for (size = PyDict_MINSIZE;
            size < minsize && size > 0;
            size <<= 1)
        ;
    return size;
#endif
}

/* estimate_keysize is reverse function of USABLE_FRACTION.
 *
 * This can be used to reserve enough size to insert n entries without
 * resizing.
 */
static inline Py_ssize_t
estimate_keysize(Py_ssize_t n)
{
    return calculate_keysize((n*3 + 1) / 2);
}

static PyDictKeysObject fd_empty_keys_struct = {
        1, /* dk_refcnt */
        1, /* dk_size */
        lookdict_unicode_nodummy, /* dk_lookup */
        0, /* dk_usable (immutable) */
        0, /* dk_nentries */
        {DKIX_EMPTY, DKIX_EMPTY, DKIX_EMPTY, DKIX_EMPTY,
         DKIX_EMPTY, DKIX_EMPTY, DKIX_EMPTY, DKIX_EMPTY}, /* dk_indices */
};

#define fd_Py_EMPTY_KEYS &fd_empty_keys_struct

/* Uncomment to check the dict content in _PyDict_CheckConsistency() */
/* #define DEBUG_PYDICT */

#ifdef DEBUG_PYDICT
#  define fd_ASSERT_CONSISTENT(op) assert(_PyAnyDict_CheckConsistency((PyObject *)(op), 1))
#else
#  define fd_ASSERT_CONSISTENT(op) assert(_PyAnyDict_CheckConsistency((PyObject *)(op), 0))
#endif


int
_PyAnyDict_CheckConsistency(PyObject *op, int check_content)
{
#define CHECK(expr) \
    do { if (!(expr)) { _PyObject_ASSERT_FAILED_MSG(op, Py_STRINGIFY(expr)); } } while (0)

    assert(op != NULL);
    CHECK(PyAnyDict_Check(op));
    PyDictObject *mp = (PyDictObject *)op;

    PyDictKeysObject *keys = mp->ma_keys;
    int splitted = _PyDict_HasSplitTable(mp);
    Py_ssize_t usable = USABLE_FRACTION(keys->dk_size);

    CHECK(0 <= mp->ma_used && mp->ma_used <= usable);
    CHECK(IS_POWER_OF_2(keys->dk_size));
    CHECK(0 <= keys->dk_usable && keys->dk_usable <= usable);
    CHECK(0 <= keys->dk_nentries && keys->dk_nentries <= usable);
    CHECK(keys->dk_usable + keys->dk_nentries <= usable);

    if (!splitted) {
        /* combined table */
        CHECK(keys->dk_refcnt == 1);
    }

    if (check_content) {
        PyDictKeyEntry *entries = DK_ENTRIES(keys);
        Py_ssize_t i;

        for (i=0; i < keys->dk_size; i++) {
            Py_ssize_t ix = dictkeys_get_index(keys, i);
            CHECK(DKIX_DUMMY <= ix && ix <= usable);
        }

        for (i=0; i < usable; i++) {
            PyDictKeyEntry *entry = &entries[i];
            PyObject *key = entry->me_key;

            if (key != NULL) {
                if (PyUnicode_CheckExact(key)) {
                    Py_hash_t hash = ((PyASCIIObject *)key)->hash;
                    CHECK(hash != -1);
                    CHECK(entry->me_hash == hash);
                }
                else {
                    /* test_dict fails if PyObject_Hash() is called again */
                    CHECK(entry->me_hash != -1);
                }
                if (!splitted) {
                    CHECK(entry->me_value != NULL);
                }
            }

            if (splitted) {
                CHECK(entry->me_value == NULL);
            }
        }

        if (splitted) {
            /* splitted table */
            for (i=0; i < mp->ma_used; i++) {
                CHECK(mp->ma_values[i] != NULL);
            }
        }
    }
    return 1;

#undef CHECK
}


static void
fd_free_keys_object(PyDictKeysObject *keys, const int decref_items)
{
    if (decref_items) {
        PyDictKeyEntry *entries = DK_ENTRIES(keys);
        Py_ssize_t i, n;
        for (i = 0, n = keys->dk_nentries; i < n; i++) {
            Py_XDECREF(entries[i].me_key);
            Py_XDECREF(entries[i].me_value);
        }
    }
    
#if PyDict_MAXFREELIST > 0
    if (keys->dk_size == PyDict_MINSIZE && numfreekeys < PyDict_MAXFREELIST) {
        keys_free_list[numfreekeys++] = keys;
        return;
    }
#endif
    PyObject_FREE(keys);
}

static PyDictKeysObject *
clone_combined_dict_keys(PyDictObject *orig)
{
    assert(PyAnyDict_Check(orig));
    assert(
        Py_TYPE(orig)->tp_iter == PyDict_Type.tp_iter
        || Py_TYPE(orig)->tp_iter == (getiterfunc)frozendict_iter
    );
    assert(orig->ma_values == NULL);
    assert(orig->ma_keys->dk_refcnt == 1);

    Py_ssize_t keys_size = _d_PyDict_KeysSize(orig->ma_keys);
    PyDictKeysObject *keys = PyObject_Malloc(keys_size);
    if (keys == NULL) {
        PyErr_NoMemory();
        return NULL;
    }

    memcpy(keys, orig->ma_keys, keys_size);

    /* After copying key/value pairs, we need to incref all
       keys and values and they are about to be co-owned by a
       new dict object. */
    PyDictKeyEntry *ep0 = DK_ENTRIES(keys);
    Py_ssize_t n = keys->dk_nentries;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyDictKeyEntry *entry = &ep0[i];
        PyObject *value = entry->me_value;
        if (value != NULL) {
            Py_INCREF(value);
            Py_INCREF(entry->me_key);
        }
    }

    /* Since we copied the keys table we now have an extra reference
       in the system.  Manually call increment _Py_RefTotal to signal that
       we have it now; calling dictkeys_incref would be an error as
       keys->dk_refcnt is already set to 1 (after memcpy). */
#ifdef Py_REF_DEBUG
    _Py_RefTotal++;
#endif
    return keys;
}

static int frozendict_resize(PyDictObject* mp, Py_ssize_t minsize) {
    const Py_ssize_t newsize = calculate_keysize(minsize);
    
    if (newsize <= 0) {
        PyErr_NoMemory();
        return -1;
    }
    
    assert(IS_POWER_OF_2(newsize));
    assert(newsize >= PyDict_MINSIZE);

    PyDictKeysObject* oldkeys = mp->ma_keys;

    /* Allocate a new table. */
    PyDictKeysObject* new_keys = new_keys_object(newsize);
    
    if (new_keys == NULL) {
        return -1;
    }
    
    // New table must be large enough.
    assert(new_keys->dk_usable >= mp->ma_used);
    
    new_keys->dk_lookup = oldkeys->dk_lookup;

    const Py_ssize_t numentries = mp->ma_used;
    PyDictKeyEntry* newentries = DK_ENTRIES(new_keys);
    
    memcpy(
        newentries, 
        DK_ENTRIES(oldkeys), 
        numentries * sizeof(PyDictKeyEntry)
    );
    
    build_indices(new_keys, newentries, numentries);
    new_keys->dk_usable -= numentries;
    new_keys->dk_nentries = numentries;
    
    // do not decref the keys inside!
    fd_dictkeys_decref(oldkeys, 0);
    
    mp->ma_keys = new_keys;
    
    return 0;
}

static int frozendict_insert(PyDictObject *mp, 
                             PyObject *key, 
                             const Py_hash_t hash, 
                             PyObject *value, 
                             int empty) {
    Py_ssize_t ix;
    PyObject *old_value;
    PyDictKeysObject* keys = mp->ma_keys;

    Py_INCREF(key);
    Py_INCREF(value);
    // MAINTAIN_TRACKING(mp, key, value);

    if (! empty) {
        ix = keys->dk_lookup(mp, key, hash, &old_value);

        if (ix == DKIX_ERROR) {
            Py_DECREF(value);
            Py_DECREF(key);
            return -1;
        }

        empty = (ix == DKIX_EMPTY);
    }

    if (empty) {
        /* Insert into new slot. */
        
        if (mp->ma_keys->dk_usable <= 0) {
            /* Need to resize. */
            if (frozendict_resize(mp, GROWTH_RATE(mp))) {
                Py_DECREF(value);
                Py_DECREF(key);
                return -1;
            }
            
            // resize changes keys
            keys = mp->ma_keys;
        }
        
        const Py_ssize_t hashpos = find_empty_slot(keys, hash);
        const Py_ssize_t dk_nentries = keys->dk_nentries;
        PyDictKeyEntry* ep = &DK_ENTRIES(keys)[dk_nentries];
        dictkeys_set_index(keys, hashpos, dk_nentries);
        ep->me_key = key;
        ep->me_hash = hash;
        ep->me_value = value;
        mp->ma_used++;
        keys->dk_usable--;
        keys->dk_nentries++;
        assert(keys->dk_usable >= 0);
    }
    else {
        DK_ENTRIES(mp->ma_keys)[ix].me_value = value;
        Py_DECREF(old_value); /* which **CAN** re-enter (see issue #22653) */
        Py_DECREF(key);
    }

    fd_ASSERT_CONSISTENT(mp);
    return 0;
}

static int frozendict_setitem(PyObject *op, 
                              PyObject *key, 
                              PyObject *value, 
                              int empty) {
    Py_hash_t hash;

    assert(key);
    assert(value);

    if (!PyUnicode_CheckExact(key) ||
        (hash = ((PyASCIIObject *) key)->hash) == -1) {
        hash = PyObject_Hash(key);

        if (hash == -1) {
            return -1;
        }
    }
    return frozendict_insert((PyDictObject*) op, key, hash, value, empty);
}

int _PyFrozendict_SetItem(PyObject *op, 
                         PyObject *key, 
                         PyObject *value, 
                         int empty) {
    if (! PyAnyFrozenDict_Check(op)) {
        PyErr_BadInternalCall();
        return -1;
    }

    return frozendict_setitem(op, key, value, empty);
}

/* Internal version of frozendict_next that returns a hash value in addition
 * to the key and value.
 * Return 1 on success, return 0 when the reached the end of the dictionary
 * (or if op is not a dictionary)
 */
static int
_frozendict_next(PyObject *op, Py_ssize_t *ppos, PyObject **pkey,
             PyObject **pvalue, Py_hash_t *phash)
{
    Py_ssize_t i;
    PyDictObject *mp;
    PyDictKeyEntry *entry_ptr;
    PyObject *value;

    if (!PyAnyDict_Check(op))
        return 0;
    mp = (PyDictObject *)op;
    i = *ppos;
    if (mp->ma_values) {
        if (i < 0 || i >= mp->ma_used)
            return 0;
        /* values of split table is always dense */
        entry_ptr = &DK_ENTRIES(mp->ma_keys)[i];
        value = mp->ma_values[i];
        assert(value != NULL);
    }
    else {
        Py_ssize_t n = mp->ma_keys->dk_nentries;
        if (i < 0 || i >= n)
            return 0;
        entry_ptr = &DK_ENTRIES(mp->ma_keys)[i];
        while (i < n && entry_ptr->me_value == NULL) {
            entry_ptr++;
            i++;
        }
        if (i >= n)
            return 0;
        value = entry_ptr->me_value;
    }
    *ppos = i+1;
    if (pkey)
        *pkey = entry_ptr->me_key;
    if (phash)
        *phash = entry_ptr->me_hash;
    if (pvalue)
        *pvalue = value;
    return 1;
}

static int
frozendict_next(PyObject *op, Py_ssize_t *ppos, PyObject **pkey, PyObject **pvalue)
{
    return _frozendict_next(op, ppos, pkey, pvalue, NULL);
}

// empty frozendict singleton
static PyObject* empty_frozendict = NULL;

static PyObject* _frozendict_new(
    PyTypeObject* type, 
    PyObject* args, 
    PyObject* kwds, 
    const int use_empty_frozendict
);

/* Internal version of PyDict_Next that returns a hash value in addition
 * to the key and value.
 * Return 1 on success, return 0 when the reached the end of the dictionary
 * (or if op is not a dictionary)
 */
static int
_fd_PyDict_Next(PyObject *op, Py_ssize_t *ppos, PyObject **pkey,
             PyObject **pvalue, Py_hash_t *phash)
{
    Py_ssize_t i;
    PyDictObject *mp;
    PyDictKeyEntry *entry_ptr;
    PyObject *value;

    if (!PyAnyDict_Check(op))
        return 0;
    mp = (PyDictObject *)op;
    i = *ppos;
    if (mp->ma_values) {
        if (i < 0 || i >= mp->ma_used)
            return 0;
        /* values of split table is always dense */
        entry_ptr = &DK_ENTRIES(mp->ma_keys)[i];
        value = mp->ma_values[i];
        assert(value != NULL);
    }
    else {
        Py_ssize_t n = mp->ma_keys->dk_nentries;
        if (i < 0 || i >= n)
            return 0;
        entry_ptr = &DK_ENTRIES(mp->ma_keys)[i];
        while (i < n && entry_ptr->me_value == NULL) {
            entry_ptr++;
            i++;
        }
        if (i >= n)
            return 0;
        value = entry_ptr->me_value;
    }
    *ppos = i+1;
    if (pkey)
        *pkey = entry_ptr->me_key;
    if (phash)
        *phash = entry_ptr->me_hash;
    if (pvalue)
        *pvalue = value;
    return 1;
}

static PyObject *
frozendict_fromkeys_impl(PyObject *type, PyObject *iterable, PyObject *value)
{
    PyObject *it;       /* iter(iterable) */
    PyObject *key;
    PyObject *d;
    int status;
    
    d = _frozendict_new(&PyFrozenDict_Type, NULL, NULL, 0);

    if (d == NULL)
        return NULL;
    
    Py_ssize_t size;
    PyDictObject *mp = (PyDictObject *)d;
    mp->ma_keys = new_keys_object(PyDict_MINSIZE);
    
    if (PyAnyDict_CheckExact(iterable)) {
        PyObject *oldvalue;
        Py_ssize_t pos = 0;
        PyObject *key;
        Py_hash_t hash;
        
        size = PyDict_GET_SIZE(iterable);
        
        if (mp->ma_keys->dk_usable < size) {
            if (frozendict_resize(mp, estimate_keysize(size))) {
                Py_DECREF(d);
                return NULL;
            }
        }

        while (_fd_PyDict_Next(iterable, &pos, &key, &oldvalue, &hash)) {
            if (frozendict_insert(mp, key, hash, value, 0)) {
                Py_DECREF(d);
                return NULL;
            }
        }
        return d;
    }
    else if (PyAnySet_CheckExact(iterable)) {
        Py_ssize_t pos = 0;
        PyObject *key;
        Py_hash_t hash;
        
        size = PySet_GET_SIZE(iterable);
        
        if (mp->ma_keys->dk_usable < size) {
            if (frozendict_resize(mp, estimate_keysize(size))) {
                Py_DECREF(d);
                return NULL;
            }
        }

        while (_PySet_NextEntry(iterable, &pos, &key, &hash)) {
            if (frozendict_insert(mp, key, hash, value, 0)) {
                Py_DECREF(d);
                return NULL;
            }
        }
    }
    else {
        it = PyObject_GetIter(iterable);
        if (it == NULL){
            Py_DECREF(d);
            return NULL;
        }

        while ((key = PyIter_Next(it)) != NULL) {
            status = frozendict_setitem(d, key, value, 0);
            Py_DECREF(key);
            if (status < 0) {
                Py_DECREF(it);
                Py_DECREF(d);
                return NULL;
            }
        }

        Py_DECREF(it);
        
        if (PyErr_Occurred()) {
            Py_DECREF(d);
            return NULL;
        }
    }
    
    fd_ASSERT_CONSISTENT(mp);

    if ((PyTypeObject*) type == &PyFrozenDict_Type || (PyTypeObject*) type == &PyCoold_Type) {
        return d;
    }

    PyObject* args = PyTuple_New(1);

    if (args == NULL) {
        Py_DECREF(d);
        return NULL;
    }

    PyTuple_SET_ITEM(args, 0, d);
    
    return PyObject_Call(type, args, NULL);
}

static PyObject *
frozendict_fromkeys(PyTypeObject *type, PyObject *const *args, Py_ssize_t nargs)
{
    PyObject *return_value = NULL;
    PyObject *iterable;
    PyObject *value = Py_None;

    if (!_PyArg_CheckPositional("fromkeys", nargs, 1, 2)) {
        goto exit;
    }
    iterable = args[0];
    if (nargs < 2) {
        goto skip_optional;
    }
    value = args[1];
skip_optional:
    return_value = frozendict_fromkeys_impl((PyObject *)type, iterable, value);

exit:
    return return_value;
}

/* Methods */

static PyObject *
fd_dict_repr(PyDictObject *mp)
{
    Py_ssize_t i;
    PyObject *key = NULL, *value = NULL;
    _PyUnicodeWriter writer;
    int first;

    i = Py_ReprEnter((PyObject *)mp);
    if (i != 0) {
        return i > 0 ? PyUnicode_FromString("{...}") : NULL;
    }

    if (mp->ma_used == 0) {
        Py_ReprLeave((PyObject *)mp);
        return PyUnicode_FromString("{}");
    }

    _PyUnicodeWriter_Init(&writer);
    writer.overallocate = 1;
    /* "{" + "1: 2" + ", 3: 4" * (len - 1) + "}" */
    writer.min_length = 1 + 4 + (2 + 4) * (mp->ma_used - 1) + 1;

    if (_PyUnicodeWriter_WriteChar(&writer, '{') < 0)
        goto error;

    /* Do repr() on each key+value pair, and insert ": " between them.
       Note that repr may mutate the dict. */
    i = 0;
    first = 1;
    while (frozendict_next((PyObject *)mp, &i, &key, &value)) {
        PyObject *s;
        int res;

        /* Prevent repr from deleting key or value during key format. */
        Py_INCREF(key);
        Py_INCREF(value);

        if (!first) {
            if (_PyUnicodeWriter_WriteASCIIString(&writer, ", ", 2) < 0)
                goto error;
        }
        first = 0;

        s = PyObject_Repr(key);
        if (s == NULL)
            goto error;
        res = _PyUnicodeWriter_WriteStr(&writer, s);
        Py_DECREF(s);
        if (res < 0)
            goto error;

        if (_PyUnicodeWriter_WriteASCIIString(&writer, ": ", 2) < 0)
            goto error;

        s = PyObject_Repr(value);
        if (s == NULL)
            goto error;
        res = _PyUnicodeWriter_WriteStr(&writer, s);
        Py_DECREF(s);
        if (res < 0)
            goto error;

        Py_CLEAR(key);
        Py_CLEAR(value);
    }

    writer.overallocate = 0;
    if (_PyUnicodeWriter_WriteChar(&writer, '}') < 0)
        goto error;

    Py_ReprLeave((PyObject *)mp);

    return _PyUnicodeWriter_Finish(&writer);

error:
    Py_ReprLeave((PyObject *)mp);
    _PyUnicodeWriter_Dealloc(&writer);
    Py_XDECREF(key);
    Py_XDECREF(value);
    return NULL;
}

#define REPR_GENERIC_START "("
#define REPR_GENERIC_END ")"
#define FROZENDICT_CLASS_NAME "frozendict"
#define COOLD_CLASS_NAME "coold"
#define REPR_GENERIC_START_LEN 1
#define REPR_GENERIC_END_LEN 1

static PyObject* frozendict_repr(PyFrozenDictObject* mp) {
    PyObject* dict_repr_res = fd_dict_repr((PyDictObject*) mp);

    if (dict_repr_res == NULL) {
        return NULL;
    }

    _PyUnicodeWriter writer;
    _PyUnicodeWriter_Init(&writer);

    int error = 0;

    PyObject* o = (PyObject*) mp;

    Py_ReprEnter(o);

    PyTypeObject* type = Py_TYPE(mp);
    size_t frozendict_name_len = strlen(type->tp_name);
    
    writer.min_length = (
        frozendict_name_len + 
        REPR_GENERIC_START_LEN + 
        PyObject_Length(dict_repr_res) + 
        REPR_GENERIC_END_LEN
    );

    if (_PyUnicodeWriter_WriteASCIIString(
        &writer, 
        type->tp_name, 
        frozendict_name_len
    )) {
        error = 1;
    }
    else {
        if (_PyUnicodeWriter_WriteASCIIString(
            &writer, 
            REPR_GENERIC_START, 
            REPR_GENERIC_START_LEN
        )) {
            error = 1;
        }
        else {
            if (_PyUnicodeWriter_WriteStr(&writer, dict_repr_res)) {
                error = 1;
            }
            else {
                error = _PyUnicodeWriter_WriteASCIIString(
                    &writer, 
                    REPR_GENERIC_END, 
                    REPR_GENERIC_END_LEN
                );
            }
        }
    }

    Py_ReprLeave(o);

    if (error) {
        _PyUnicodeWriter_Dealloc(&writer);
        return NULL;
    }

    return _PyUnicodeWriter_Finish(&writer);
}

static PyObject *
fd_dict_subscript(PyDictObject *mp, PyObject *key)
{
    Py_ssize_t ix;
    Py_hash_t hash;
    PyObject *value;

    if (!PyUnicode_CheckExact(key) ||
        (hash = ((PyASCIIObject *) key)->hash) == -1) {
        hash = PyObject_Hash(key);
        if (hash == -1)
            return NULL;
    }
    ix = (mp->ma_keys->dk_lookup)(mp, key, hash, &value);
    if (ix == DKIX_ERROR)
        return NULL;
    if (ix == DKIX_EMPTY || value == NULL) {
        if (!PyAnyDict_CheckExact(mp)) {
            /* Look up __missing__ method if we're a subclass. */
            PyObject *missing, *res;
            _Py_IDENTIFIER(__missing__);
            missing = _PyObject_LookupSpecial((PyObject *)mp, &PyId___missing__);
            if (missing != NULL) {
                res = PyObject_CallOneArg(missing, key);
                Py_DECREF(missing);
                return res;
            }
            else if (PyErr_Occurred())
                return NULL;
        }
        _PyErr_SetKeyError(key);
        return NULL;
    }
    Py_INCREF(value);
    return value;
}

static PyMappingMethods frozendict_as_mapping = {
    (lenfunc)dict_length, /*mp_length*/
    (binaryfunc)fd_dict_subscript, /*mp_subscript*/
};

static int frozendict_merge(PyObject* a, PyObject* b, int empty) {
    /* We accept for the argument either a concrete dictionary object,
     * or an abstract "mapping" object.  For the former, we can do
     * things quite efficiently.  For the latter, we only require that
     * PyMapping_Keys() and PyObject_GetItem() be supported.
     */
    assert(a != NULL);
    assert(PyAnyFrozenDict_Check(a));
    assert(b != NULL);
    
    PyDictObject* mp = (PyDictObject*) a;
    
    if (
        PyAnyDict_Check(b) && 
        (
            Py_TYPE(b)->tp_iter == PyDict_Type.tp_iter ||
            Py_TYPE(b)->tp_iter == (getiterfunc)frozendict_iter
        )
    ) {
        PyDictObject* other = (PyDictObject*)b;
        const Py_ssize_t numentries = other->ma_used;
        
        if (other == mp || numentries == 0) {
            /* a.update(a) or a.update({}); nothing to do */
            return 0;
        }
        
        const int is_other_combined = other->ma_values == NULL;
        
        PyDictKeysObject* okeys = other->ma_keys;
        
        if (
            empty 
            && is_other_combined 
            && numentries == okeys->dk_nentries 
            && (
                okeys->dk_size == PyDict_MINSIZE 
                || okeys->dk_nentries == other->ma_used
            )
        ) {
            PyDictKeysObject *keys = clone_combined_dict_keys(other);
            if (keys == NULL) {
                return -1;
            }

            mp->ma_keys = keys;
            
            mp->ma_used = numentries;
            mp->ma_version_tag = DICT_NEXT_VERSION();
            fd_ASSERT_CONSISTENT(mp);

            // if (_PyObject_GC_IS_TRACKED(other) && !_PyObject_GC_IS_TRACKED(mp)) {
            //     PyObject_GC_Track(mp);
            // }
            
            return 0;
        }
        
        PyDictKeyEntry* ep0 = DK_ENTRIES(okeys);
        PyDictKeyEntry* entry;
        PyObject* key;
        PyObject* value;
        Py_hash_t hash;
        int err;
        
        if (mp->ma_keys == NULL) {
            mp->ma_keys = new_keys_object(PyDict_MINSIZE);
        }

        /* Do one big resize at the start, rather than
         * incrementally resizing as we insert new items.  Expect
         * that there will be no (or few) overlapping keys.
         */
        if (mp->ma_keys->dk_usable < numentries) {
            if (frozendict_resize(mp, estimate_keysize(mp->ma_used + numentries))) {
               return -1;
            }
        }
        
        for (Py_ssize_t i = 0, n = okeys->dk_nentries; i < n; i++) {
            entry = &ep0[i];
            key = entry->me_key;
            hash = entry->me_hash;
            
            if (is_other_combined) {
                value = entry->me_value;
            }
            else {
                value = other->ma_values[i];
            }
            
            if (value != NULL) {
                Py_INCREF(key);
                Py_INCREF(value);
                err = frozendict_insert(mp, key, hash, value, empty);
                Py_DECREF(value);
                Py_DECREF(key);
                
                if (err != 0) {
                    return -1;
                }
                
                if (n != other->ma_keys->dk_nentries) {
                    PyErr_SetString(PyExc_RuntimeError,
                                    "dict mutated during update");
                    return -1;
                }
            }
        }
    }
    else {
        /* Do it the generic, slower way */
        PyObject *keys = PyMapping_Keys(b);
        PyObject *iter;
        PyObject *key, *value;
        int status;
        
        if (mp->ma_keys == NULL) {
            mp->ma_keys = new_keys_object(PyDict_MINSIZE);
        }

        if (keys == NULL)
            /* Docstring says this is equivalent to E.keys() so
             * if E doesn't have a .keys() method we want
             * AttributeError to percolate up.  Might as well
             * do the same for any other error.
             */
            return -1;

        iter = PyObject_GetIter(keys);
        Py_DECREF(keys);
        if (iter == NULL)
            return -1;

        for (key = PyIter_Next(iter); key; key = PyIter_Next(iter)) {
            value = PyObject_GetItem(b, key);
            if (value == NULL) {
                Py_DECREF(iter);
                Py_DECREF(key);
                return -1;
            }
            status = frozendict_setitem(a, key, value, 0);
            Py_DECREF(key);
            Py_DECREF(value);
            if (status < 0) {
                Py_DECREF(iter);
                return -1;
            }
        }
        Py_DECREF(iter);
        if (PyErr_Occurred())
            /* Iterator completed, via error */
            return -1;
    }
    fd_ASSERT_CONSISTENT(a);
    return 0;
}

static int frozendict_merge_from_seq2(PyObject* d, PyObject* seq2) {
    assert(d != NULL);
    assert(PyAnyFrozenDict_Check(d));
    assert(seq2 != NULL);

    PyObject* it = PyObject_GetIter(seq2);
    
    if (it == NULL) {
        return -1;
    }

    PyObject* fast;     /* item as a 2-tuple or 2-list */
    PyObject* key = NULL;
    PyObject* value = NULL;
    Py_ssize_t n;
    PyObject* item;
    int res = 0;
    
    PyDictObject* mp = (PyDictObject*) d;

    if (mp->ma_keys == NULL) {
        mp->ma_keys = new_keys_object(PyDict_MINSIZE);
    }

    for (Py_ssize_t i = 0; ; ++i) {
        fast = NULL;
        item = PyIter_Next(it);
        
        if (item == NULL) {
            if (PyErr_Occurred()) {
                res = -1;
            }
            
            break;
        }

        /* Convert item to sequence, and verify length 2. */
        fast = PySequence_Fast(item, "");
        
        if (fast == NULL) {
            if (PyErr_ExceptionMatches(PyExc_TypeError)) {
                PyErr_Format(PyExc_TypeError,
                    "cannot convert dictionary update "
                    "sequence element #%zd to a sequence",
                    i);
            }
            Py_DECREF(item);
            res = -1;
            break;
        }
        
        n = PySequence_Fast_GET_SIZE(fast);
        
        if (n != 2) {
            PyErr_Format(PyExc_ValueError,
                         "dictionary update sequence element #%zd "
                         "has length %zd; 2 is required",
                         i, n);
            Py_DECREF(fast);
            Py_DECREF(item);
            res = -1;
            break;
        }

        /* Update/merge with this (key, value) pair. */
        key = PySequence_Fast_GET_ITEM(fast, 0);
        Py_INCREF(key);
        value = PySequence_Fast_GET_ITEM(fast, 1);
        Py_INCREF(value);
        
        if (frozendict_setitem(d, key, value, 0) < 0) {
            Py_DECREF(key);
            Py_DECREF(value);
            Py_DECREF(fast);
            Py_DECREF(item);
            res = -1;
            break;
        }
        
        Py_DECREF(key);
        Py_DECREF(value);
        Py_DECREF(fast);
        Py_DECREF(item);
    }
    
    Py_DECREF(it);
    fd_ASSERT_CONSISTENT(d);
    return res;
}

static int frozendict_update_arg(PyObject *self, 
                                 PyObject *arg, 
                                 const int empty) {
    if (PyAnyDict_CheckExact(arg)) {
        return frozendict_merge(self, arg, empty);
    }
    _Py_IDENTIFIER(keys);
    PyObject *func;
    if (_PyObject_LookupAttrId(arg, &PyId_keys, &func) < 0) {
        return -1;
    }
    if (func != NULL) {
        Py_DECREF(func);
        return frozendict_merge(self, arg, empty);
    }
    return frozendict_merge_from_seq2(self, arg);
}

static int frozendict_update_common(PyObject* self, 
                                    PyObject* arg, 
                                    PyObject* kwds) {
    int result = 0;
    const int no_arg = (arg == NULL);

    if (! no_arg) {
        result = frozendict_update_arg(self, arg, 1);
    }

    if (result == 0 && kwds != NULL) {
        if (PyArg_ValidateKeywordArguments(kwds)) {
            result = frozendict_merge(self, kwds, no_arg);
        }
        else {
            result = -1;
        }
    }
    
    return result;
}

static PyObject* frozendict_copy(PyObject* o, PyObject* Py_UNUSED(ignored)) {
    if (PyAnyFrozenDict_CheckExact(o)) {
        Py_INCREF(o);
        return o;
    }
    
    PyObject* args = PyTuple_New(1);

    if (args == NULL) {
        return NULL;
    }

    Py_INCREF(o);
    PyTuple_SET_ITEM(args, 0, o);
    
    PyTypeObject* type = Py_TYPE(o);

    return PyObject_Call((PyObject *) type, args, NULL);
}

static int frozendict_equal(PyDictObject* a, PyDictObject* b) {
    if (a == b) {
        return 1;
    }

    if (a->ma_used != b->ma_used) {
        /* can't be equal if # of entries differ */
        return 0;
    }

    PyDictKeysObject* keys = a->ma_keys;
    PyDictKeyEntry* ep;
    PyObject* aval;
    int cmp = 1;
    PyObject* bval;
    PyObject* key;

    /* Same # of entries -- check all of 'em.  Exit early on any diff. */
    for (Py_ssize_t i = 0; i < keys->dk_nentries; i++) {
        ep = &DK_ENTRIES(keys)[i];
        aval = ep->me_value;
        Py_INCREF(aval);
        key = ep->me_key;
        Py_INCREF(key);

        /* reuse the known hash value */
        b->ma_keys->dk_lookup(b, key, ep->me_hash, &bval);

        if (bval == NULL) {
            if (PyErr_Occurred()) {
                cmp = -1;
            }
            else {
                cmp = 0;
            }
        }
        else {
            Py_INCREF(bval);
            cmp = PyObject_RichCompareBool(aval, bval, Py_EQ);
            Py_DECREF(bval);
        }

        Py_DECREF(key);
        Py_DECREF(aval);
        if (cmp <= 0) { /* error or not equal */
            break;
        }
    }

    return cmp;
}

static PyObject* frozendict_richcompare(PyObject *v, PyObject *w, int op) {
    int cmp;
    PyObject *res;

    if (!PyAnyDict_Check(v) || !PyAnyDict_Check(w)) {
        res = Py_NotImplemented;
    }
    else if (op == Py_EQ || op == Py_NE) {
        cmp = frozendict_equal((PyDictObject *)v, (PyDictObject *)w);
        if (cmp < 0)
            return NULL;
        res = (cmp == (op == Py_EQ)) ? Py_True : Py_False;
    }
    else
        res = Py_NotImplemented;
    Py_INCREF(res);
    return res;
}

static Py_ssize_t dict_get_index(PyDictObject *self, PyObject *key) {
    Py_hash_t hash;
    PyObject* val;

    if (!PyUnicode_CheckExact(key) ||
        (hash = ((PyASCIIObject *) key)->hash) == -1) {
        hash = PyObject_Hash(key);
        if (hash == -1)
            return DKIX_ERROR;
    }
    return (self->ma_keys->dk_lookup) (self, key, hash, &val);
}

/* Forward */
static PyObject *frozendictkeys_new(PyObject *, PyObject *);
static PyObject *frozendictitems_new(PyObject *, PyObject *);
static PyObject *frozendictvalues_new(PyObject *, PyObject *);

static PyObject *
frozendict_reduce(PyFrozenDictObject* mp, PyObject *Py_UNUSED(ignored))
{
    PyObject* items = frozendictitems_new((PyObject*) mp, NULL);
    
    if (items == NULL) {
        return NULL;
    }
    
    PyObject* items_tuple = PySequence_Tuple(items);
    
    if (items_tuple == NULL) {
        return NULL;
    }
    
    return Py_BuildValue("O(N)", Py_TYPE(mp), items_tuple);
}

static PyObject* frozendict_clone(PyObject* self) {
    PyTypeObject* type = Py_TYPE(self);
    PyObject* new_op = type->tp_alloc(type, 0);

    if (new_op == NULL){
        return NULL;
    }

    if (type == &PyFrozenDict_Type || type == &PyCoold_Type) {
        PyObject_GC_UnTrack(new_op);
    }

    PyDictObject* mp = (PyDictObject*) self;

    PyDictKeysObject *keys = clone_combined_dict_keys(mp);
    
    if (keys == NULL) {
        return NULL;
    }

    PyFrozenDictObject* new_mp = (PyFrozenDictObject*) new_op;
    new_mp->ma_keys = keys;
    
    // if (_PyObject_GC_IS_TRACKED(mp) && !_PyObject_GC_IS_TRACKED(new_mp)) {
    //     PyObject_GC_Track(new_mp);
    // }
    
    new_mp->ma_used = mp->ma_used;
    new_mp->_hash = -1;
    new_mp->_hash_calculated = 0;
    new_mp->ma_version_tag = DICT_NEXT_VERSION();
    
    fd_ASSERT_CONSISTENT(new_mp);
    
    return new_op;
}

static PyObject* frozendict_set(
    PyObject* self, 
    PyObject* const* args, 
    Py_ssize_t nargs
) {
    PyObject* new_op = frozendict_clone(self);

    if (new_op == NULL) {
        return NULL;
    }

    PyObject* set_key = args[0];
    
    if (frozendict_setitem(new_op, set_key, args[1], 0)) {
        Py_DECREF(new_op);
        return NULL;
    }

    if (
        ((PyDictObject*) self)->ma_keys->dk_lookup == lookdict_unicode_nodummy && 
        ! PyUnicode_CheckExact(set_key)
    ) {
        ((PyFrozenDictObject*) new_op)->ma_keys->dk_lookup = lookdict;
    }
    
    return new_op;
}

static PyObject* frozendict_del(PyObject* self, 
                                PyObject *const *args, 
                                Py_ssize_t nargs) {
    if (! _PyArg_CheckPositional("del", nargs, 1, 1)) {
        return NULL;
    }

    PyDictObject* mp = (PyDictObject*) self;
    PyObject* del_key = args[0];
    const Py_ssize_t ix = dict_get_index(mp, del_key);

    if (ix == DKIX_ERROR) {
        return NULL;
    }

    if (ix == DKIX_EMPTY) {
        _PyErr_SetKeyError(del_key);
        return NULL;
    }

    PyTypeObject* type = Py_TYPE(self);
    PyObject* new_op = type->tp_alloc(type, 0);

    if (new_op == NULL) {
        return NULL;
    }

    if (type == &PyFrozenDict_Type || type == &PyCoold_Type) {
        PyObject_GC_UnTrack(new_op);
    }

    const Py_ssize_t size = mp->ma_used;
    const Py_ssize_t sizemm = size - 1;
    const Py_ssize_t newsize = estimate_keysize(sizemm);
    
    if (newsize <= 0) {
        Py_DECREF(new_op);
        PyErr_NoMemory();
        return NULL;
    }
    
    assert(IS_POWER_OF_2(newsize));
    assert(newsize >= PyDict_MINSIZE);

    /* Allocate a new table. */
    PyDictKeysObject* new_keys = new_keys_object(newsize);
    
    if (new_keys == NULL) {
        Py_DECREF(new_op);
        return NULL;
    }
    
    const PyDictKeysObject* old_keys = mp->ma_keys;
    new_keys->dk_lookup = old_keys->dk_lookup;
    
    PyFrozenDictObject* new_mp = (PyFrozenDictObject*) new_op;
    
    // New table must be large enough.
    assert(new_keys->dk_usable >= new_mp->ma_used);
    
    new_mp->ma_keys = new_keys;
    new_mp->_hash = -1;
    new_mp->_hash_calculated = 0;
    new_mp->ma_version_tag = DICT_NEXT_VERSION();

    PyObject* key;
    PyObject* value;
    Py_hash_t hash;
    Py_ssize_t hashpos;
    PyDictKeyEntry* old_entries = DK_ENTRIES(old_keys);
    PyDictKeyEntry* new_entries = DK_ENTRIES(new_keys);
    PyDictKeyEntry* old_entry;
    PyDictKeyEntry* new_entry;
    Py_ssize_t new_i;
    int deleted = 0;

    for (Py_ssize_t i = 0; i < size; i++) {
        if (i == ix) {
            deleted = 1;
            continue;
        }

        new_i = i - deleted;

        old_entry = &old_entries[i];
        hash = old_entry->me_hash;
        key = old_entry->me_key;
        value = old_entry->me_value;
        Py_INCREF(key);
        Py_INCREF(value);
        hashpos = find_empty_slot(new_keys, hash);
        dictkeys_set_index(new_keys, hashpos, new_i);
        new_entry = &new_entries[new_i];
        new_entry->me_key = key;
        new_entry->me_hash = hash;
        new_entry->me_value = value;
    }

    new_mp->ma_used = sizemm;
    new_keys->dk_usable -= sizemm;
    new_keys->dk_nentries = sizemm;

    fd_ASSERT_CONSISTENT(new_mp);
    
    return new_op;
}

static PyMethodDef frozen_mapp_methods[] = {
    DICT___CONTAINS___METHODDEF
    {"__getitem__", (PyCFunction)(void(*)(void))fd_dict_subscript,        METH_O | METH_COEXIST,
     getitem__doc__},
    {"__sizeof__",      (PyCFunction)(void(*)(void))dict_sizeof,       METH_NOARGS,
     sizeof__doc__},
    DICT_GET_METHODDEF
    {"keys",            frozendictkeys_new,             METH_NOARGS,
    keys__doc__},
    {"items",           frozendictitems_new,            METH_NOARGS,
    items__doc__},
    {"values",          frozendictvalues_new,           METH_NOARGS,
    values__doc__},
    {"fromkeys",        (PyCFunction)(void(*)(void))frozendict_fromkeys, METH_FASTCALL|METH_CLASS, 
    dict_fromkeys__doc__},
    {"copy",            (PyCFunction)frozendict_copy,   METH_NOARGS,
     copy__doc__},
    DICT___REVERSED___METHODDEF
    {"__class_getitem__", Py_GenericAlias, METH_O|METH_CLASS, "See PEP 585"},
    {"__reduce__", (PyCFunction)(void(*)(void))frozendict_reduce, METH_NOARGS,
     ""},
    {NULL,              NULL}   /* sentinel */
};

PyDoc_STRVAR(frozendict_set_doc,
"set($self, key, value, /)\n"
"--\n"
"\n"
"Returns a copy of the dictionary with the new (key, value) item.   ");

PyDoc_STRVAR(frozendict_del_doc,
"del($self, key, /)\n"
"--\n"
"\n"
"Returns a copy of the dictionary without the item of the corresponding key.   ");

static PyMethodDef coold_mapp_methods[] = {
    DICT___CONTAINS___METHODDEF
    {"__getitem__", (PyCFunction)(void(*)(void))fd_dict_subscript,        METH_O | METH_COEXIST,
     getitem__doc__},
    {"__sizeof__",      (PyCFunction)(void(*)(void))dict_sizeof,       METH_NOARGS,
     sizeof__doc__},
    DICT_GET_METHODDEF
    {"keys",            frozendictkeys_new,             METH_NOARGS,
    keys__doc__},
    {"items",           frozendictitems_new,            METH_NOARGS,
    items__doc__},
    {"values",          frozendictvalues_new,           METH_NOARGS,
    values__doc__},
    {"fromkeys",        (PyCFunction)(void(*)(void))frozendict_fromkeys, METH_FASTCALL|METH_CLASS, 
    dict_fromkeys__doc__},
    {"copy",            (PyCFunction)frozendict_copy,   METH_NOARGS,
     copy__doc__},
    DICT___REVERSED___METHODDEF
     {"__reduce__", (PyCFunction)(void(*)(void))frozendict_reduce, METH_NOARGS,
     ""},
    {"set",             (PyCFunction)(void(*)(void))
                        frozendict_set,                 METH_FASTCALL,
    frozendict_set_doc},
    {"delete",          (PyCFunction)(void(*)(void))
                        frozendict_del,                 METH_FASTCALL,
    frozendict_del_doc},
    {NULL,              NULL}   /* sentinel */
};

static PyObject* frozendict_new_barebone(PyTypeObject* type) {
    PyObject* self = type->tp_alloc(type, 0);
    
    if (self == NULL) {
        return NULL;
    }

    /* The object has been implicitly tracked by tp_alloc */
    if (type == &PyFrozenDict_Type || type == &PyCoold_Type) {
        PyObject_GC_UnTrack(self);
    }

    PyFrozenDictObject* mp = (PyFrozenDictObject*) self;

    mp->ma_keys = NULL;
    mp->ma_values = NULL;
    mp->ma_used = 0;
    mp->_hash = -1;
    mp->_hash_calculated = 0;

    return self;
}

static PyObject* frozendict_vectorcall(PyObject* type, 
                                       PyObject* const* args,
                                       size_t nargsf, 
                                       PyObject* kwnames) {
    assert(PyType_Check(type));
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    
    if (! _PyArg_CheckPositional("dict", nargs, 0, 1)) {
        return NULL;
    }
    
    PyTypeObject* ttype = (PyTypeObject*) type;
    
    Py_ssize_t size;
    PyObject* arg = NULL;

    if (nargs == 1) {
        arg = args[0];

        // only argument is a frozendict
        if (
            arg != NULL 
            && PyAnyFrozenDict_CheckExact(arg) 
            && (
                ttype == &PyFrozenDict_Type
                || ttype == &PyCoold_Type
            )
        ) {
            size = (
                kwnames == NULL
                ? 0
                : PyTuple_GET_SIZE(kwnames)
            );

            if (size == 0) {
                Py_INCREF(arg);

                return arg;
            }
        }
    }

    PyObject* self = frozendict_new_barebone(ttype);

    PyFrozenDictObject* mp = (PyFrozenDictObject*) self;
    
    int empty = 1;

    if (nargs == 1) {
        empty = 0;

        if (frozendict_update_arg(self, arg, 1) < 0) {
            Py_DECREF(self);
            return NULL;
        }
        
        args++;
    }

    if (kwnames != NULL) {
        if (mp->ma_keys == NULL) {
            mp->ma_keys = new_keys_object(PyDict_MINSIZE);
        }

        size = (
            kwnames == NULL
            ? 0
            : PyTuple_GET_SIZE(kwnames)
        );

        if (mp->ma_keys->dk_usable < size) {
            if (frozendict_resize((PyDictObject*) self, estimate_keysize(mp->ma_used + size))) {
               return NULL;
            }
        }
        
        for (Py_ssize_t i = 0; i < size; i++) {
            if (frozendict_setitem(self, PyTuple_GET_ITEM(kwnames, i), args[i], empty) < 0) {
                Py_DECREF(self);
                return NULL;
            }
        }
    }
    
    // if frozendict is empty, return the empty singleton
    if (mp->ma_used == 0) {
        if (ttype == &PyFrozenDict_Type || ttype == &PyCoold_Type) {
            if (empty_frozendict == NULL) {
                empty_frozendict = self;
                Py_INCREF(fd_Py_EMPTY_KEYS);
                ((PyDictObject*) empty_frozendict)->ma_keys = fd_Py_EMPTY_KEYS;
                mp->ma_version_tag = DICT_NEXT_VERSION();
            }
            
            Py_INCREF(empty_frozendict);

            return empty_frozendict;
        }
        else {
            Py_INCREF(fd_Py_EMPTY_KEYS);
            mp->ma_keys = fd_Py_EMPTY_KEYS;
        }
    }
    
    mp->ma_version_tag = DICT_NEXT_VERSION();
    
    fd_ASSERT_CONSISTENT(mp);
    
    return self;
}

static PyObject* _frozendict_new(
    PyTypeObject* type, 
    PyObject* args, 
    PyObject* kwds, 
    const int use_empty_frozendict
) {
    assert(type != NULL && type->tp_alloc != NULL);
    
    PyObject* arg = NULL;
    
    if (args != NULL && ! PyArg_UnpackTuple(args, "dict", 0, 1, &arg)) {
        return NULL;
    }
    
    const int arg_is_frozendict = (arg != NULL && PyAnyFrozenDict_CheckExact(arg));
    const int kwds_size = ((kwds != NULL) 
        ? ((PyDictObject*) kwds)->ma_used 
        : 0
    );
    
    // only argument is a frozendict
    if (arg_is_frozendict && kwds_size == 0 && (type == &PyFrozenDict_Type || type == &PyCoold_Type)) {
        Py_INCREF(arg);
        
        return arg;
    }
    
    PyObject* self = frozendict_new_barebone(type);

    PyFrozenDictObject* mp = (PyFrozenDictObject*) self;
    
    if (frozendict_update_common(self, arg, kwds)) {
        Py_DECREF(self);
        return NULL;
    }
    
    // if frozendict is empty, return the empty singleton
    if (mp->ma_used == 0) {
        if (
            use_empty_frozendict && 
            (type == &PyFrozenDict_Type || type == &PyCoold_Type)
        ) {
            if (empty_frozendict == NULL) {
                empty_frozendict = self;
                Py_INCREF(fd_Py_EMPTY_KEYS);
                ((PyDictObject*) empty_frozendict)->ma_keys = fd_Py_EMPTY_KEYS;
                mp->ma_version_tag = DICT_NEXT_VERSION();
            }
            
            Py_INCREF(empty_frozendict);

            return empty_frozendict;
        }
        else {
            Py_INCREF(fd_Py_EMPTY_KEYS);
            mp->ma_keys = fd_Py_EMPTY_KEYS;
        }
    }
    
    mp->ma_version_tag = DICT_NEXT_VERSION();
    
    return self;
}

static PyObject* frozendict_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
    return _frozendict_new(type, args, kwds, 1);
}

#define MINUSONE_HASH ((Py_hash_t) -1)

static Py_hash_t frozendict_hash(PyObject* self) {
    PyFrozenDictObject* frozen_self = (PyFrozenDictObject*) self;
    Py_hash_t hash;

    if (frozen_self->_hash_calculated) {
        hash = frozen_self->_hash;
        
        if (hash == MINUSONE_HASH) {
            PyErr_SetObject(PyExc_TypeError, Py_None);
        }
    }
    else {
        PyObject* frozen_items_tmp = frozendictitems_new(self, NULL);

        if (frozen_items_tmp == NULL) {
            hash = MINUSONE_HASH;
        }
        else {
            PyObject* frozen_items = PyFrozenSet_New(frozen_items_tmp);

            if (frozen_items == NULL) {
                hash = MINUSONE_HASH;
            }
            else {
                hash = PyFrozenSet_Type.tp_hash(frozen_items);
            }
        }

        frozen_self->_hash = hash;
        frozen_self->_hash_calculated = 1;
    }

    return hash;
}

static PyObject* frozendict_or(PyObject *self, PyObject *other) {
    if (! PyAnyFrozenDict_Check(self) || ! PyAnyDict_Check(other)) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    PyObject* new = frozendict_clone(self);

    if (new == NULL) {
        return NULL;
    }

    if (frozendict_update_arg(new, other, 0)) {
        Py_DECREF(new);
        return NULL;
    }

    return new;
}

static PyNumberMethods frozendict_as_number = {
    .nb_or = frozendict_or,
};

PyDoc_STRVAR(frozendict_doc,
"An immutable version of dict.\n"
"\n"
FROZENDICT_CLASS_NAME "() -> returns an empty immutable dictionary\n"
FROZENDICT_CLASS_NAME "(mapping) -> returns an immutable dictionary initialized from a mapping object's\n"
"    (key, value) pairs\n"
FROZENDICT_CLASS_NAME "(iterable) -> returns an immutable dictionary, equivalent to:\n"
"    d = {}\n"
"    "
"    for k, v in iterable:\n"
"        d[k] = v\n"
"    "
"    " FROZENDICT_CLASS_NAME "(d)\n"
FROZENDICT_CLASS_NAME "(**kwargs) -> returns an immutable dictionary initialized with the name=value pairs\n"
"    in the keyword argument list.  For example:  " FROZENDICT_CLASS_NAME "(one=1, two=2)");

PyDoc_STRVAR(coold_doc,
"An immutable version of dict.\n"
"\n"
COOLD_CLASS_NAME "() -> returns an empty immutable dictionary\n"
COOLD_CLASS_NAME "(mapping) -> returns an immutable dictionary initialized from a mapping object's\n"
"    (key, value) pairs\n"
COOLD_CLASS_NAME "(iterable) -> returns an immutable dictionary, equivalent to:\n"
"    d = {}\n"
"    "
"    for k, v in iterable:\n"
"        d[k] = v\n"
"    "
"    " COOLD_CLASS_NAME "(d)\n"
COOLD_CLASS_NAME "(**kwargs) -> returns an immutable dictionary initialized with the name=value pairs\n"
"    in the keyword argument list.  For example:  " COOLD_CLASS_NAME "(one=1, two=2)");

PyTypeObject PyFrozenDict_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "frozendict." FROZENDICT_CLASS_NAME,        /* tp_name */
    sizeof(PyFrozenDictObject),                 /* tp_basicsize */
    0,                                          /* tp_itemsize */
    (destructor)dict_dealloc,                   /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_as_async */
    (reprfunc)frozendict_repr,                  /* tp_repr */
    &frozendict_as_number,                      /* tp_as_number */
    &dict_as_sequence,                          /* tp_as_sequence */
    &frozendict_as_mapping,                     /* tp_as_mapping */
    (hashfunc)frozendict_hash,                  /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC
        | Py_TPFLAGS_BASETYPE,                  /* tp_flags */
    frozendict_doc,                             /* tp_doc */
    dict_traverse,                              /* tp_traverse */
    0,                                          /* tp_clear */
    frozendict_richcompare,                     /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    (getiterfunc)frozendict_iter,               /* tp_iter */
    0,                                          /* tp_iternext */
    frozen_mapp_methods,                        /* tp_methods */
    0,                                          /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    PyType_GenericAlloc,                        /* tp_alloc */
    frozendict_new,                             /* tp_new */
    PyObject_GC_Del,                            /* tp_free */
    .tp_vectorcall = frozendict_vectorcall,
};

PyTypeObject PyCoold_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "frozendict." COOLD_CLASS_NAME,             /* tp_name */
    sizeof(PyFrozenDictObject),                 /* tp_basicsize */
    0,                                          /* tp_itemsize */
    (destructor)dict_dealloc,                   /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_as_async */
    (reprfunc)frozendict_repr,                  /* tp_repr */
    &frozendict_as_number,                      /* tp_as_number */
    &dict_as_sequence,                          /* tp_as_sequence */
    &frozendict_as_mapping,                     /* tp_as_mapping */
    (hashfunc)frozendict_hash,                  /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC
        | Py_TPFLAGS_BASETYPE,                  /* tp_flags */
    coold_doc,                                  /* tp_doc */
    dict_traverse,                              /* tp_traverse */
    0,                                          /* tp_clear */
    frozendict_richcompare,                     /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    (getiterfunc)frozendict_iter,               /* tp_iter */
    0,                                          /* tp_iternext */
    coold_mapp_methods,                         /* tp_methods */
    0,                                          /* tp_members */
    0,                                          /* tp_getset */
    &PyFrozenDict_Type,                         /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    PyType_GenericAlloc,                        /* tp_alloc */
    frozendict_new,                             /* tp_new */
    PyObject_GC_Del,                            /* tp_free */
    .tp_vectorcall = frozendict_vectorcall,
};

/* Dictionary iterator types */
static PyObject *
fd_dictiter_new(PyDictObject *dict, PyTypeObject *itertype)
{
    dictiterobject *di;
    di = PyObject_GC_New(dictiterobject, itertype);
    if (di == NULL) {
        return NULL;
    }
    Py_INCREF(dict);
    di->di_dict = dict;
    di->di_used = dict->ma_used;
    di->len = dict->ma_used;
    if (itertype == &PyDictRevIterKey_Type ||
         itertype == &PyDictRevIterItem_Type ||
         itertype == &PyDictRevIterValue_Type) {
        if (dict->ma_values) {
            di->di_pos = dict->ma_used - 1;
        }
        else {
            di->di_pos = dict->ma_keys->dk_nentries - 1;
        }
    }
    else {
        di->di_pos = 0;
    }
    if (itertype == &PyFrozenDictIterItem_Type ||
        itertype == &PyDictRevIterItem_Type) {
        di->di_result = PyTuple_Pack(2, Py_None, Py_None);
        if (di->di_result == NULL) {
            Py_DECREF(di);
            return NULL;
        }
    }
    else {
        di->di_result = NULL;
    }
    PyObject_GC_Track(di);
    return (PyObject *)di;
}

static PyObject* frozendict_iter(PyDictObject *dict) {
    return fd_dictiter_new(dict, &PyFrozenDictIterKey_Type);
}

static PyObject* frozendictiter_iternextkey(dictiterobject* di) {
    Py_ssize_t pos = di->di_pos;
    assert(pos >= 0);
    PyDictObject* d = di->di_dict;
    assert(d != NULL);

    if (pos >= d->ma_used) {
        return NULL;
    }

    assert(PyAnyFrozenDict_Check(d));
    PyObject* key = DK_ENTRIES(d->ma_keys)[di->di_pos].me_key;
    assert(key != NULL);
    di->di_pos++;
    di->len--;
    Py_INCREF(key);
    return key;
}

PyTypeObject PyFrozenDictIterKey_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "frozendict_keyiterator",                   /* tp_name */
    sizeof(dictiterobject),               /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)dictiter_dealloc,               /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_as_async */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,    /* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)dictiter_traverse,            /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    PyObject_SelfIter,                          /* tp_iter */
    (iternextfunc)frozendictiter_iternextkey,   /* tp_iternext */
    dictiter_methods,                           /* tp_methods */
    0,
};

static PyObject* frozendictiter_iternextvalue(dictiterobject* di) {
    Py_ssize_t pos = di->di_pos;
    assert(pos >= 0);
    PyDictObject* d = di->di_dict;
    assert(d != NULL);

    if (pos >= d->ma_used) {
        return NULL;
    }

    assert(PyAnyFrozenDict_Check(d));
    PyObject* val = DK_ENTRIES(d->ma_keys)[di->di_pos].me_value;
    assert(val != NULL);
    di->di_pos++;
    di->len--;
    Py_INCREF(val);
    return val;
}

PyTypeObject PyFrozenDictIterValue_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "frozendict_valueiterator",                 /* tp_name */
    sizeof(dictiterobject),                     /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)dictiter_dealloc,               /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_as_async */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,    /* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)dictiter_traverse,            /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    PyObject_SelfIter,                          /* tp_iter */
    (iternextfunc)frozendictiter_iternextvalue, /* tp_iternext */
    dictiter_methods,                           /* tp_methods */
    0,
};

static PyObject* frozendictiter_iternextitem(dictiterobject* di) {
    Py_ssize_t pos = di->di_pos;
    assert(pos >= 0);
    PyDictObject* d = di->di_dict;
    assert(d != NULL);

    if (pos >= d->ma_used) {
        return NULL;
    }

    assert(PyAnyFrozenDict_Check(d));
    PyDictKeyEntry* entry_ptr = &DK_ENTRIES(d->ma_keys)[di->di_pos];
    PyObject* key = entry_ptr->me_key;
    PyObject* val = entry_ptr->me_value;
    assert(key != NULL);
    assert(val != NULL);
    di->di_pos++;
    Py_INCREF(key);
    Py_INCREF(val);

    PyObject* result;
    if (Py_REFCNT(di->di_result) == 1) {
        result = di->di_result;
        PyObject *oldkey = PyTuple_GET_ITEM(result, 0);
        PyObject *oldvalue = PyTuple_GET_ITEM(result, 1);
        Py_INCREF(result);
        Py_DECREF(oldkey);
        Py_DECREF(oldvalue);

        // if (!_PyObject_GC_IS_TRACKED(result)) {
        //     PyObject_GC_Track(result);
        // }
    }
    else {
        result = PyTuple_New(2);
        if (result == NULL)
            return NULL;
    }

    PyTuple_SET_ITEM(result, 0, key);  /* steals reference */
    PyTuple_SET_ITEM(result, 1, val);  /* steals reference */
    return result;
}

PyTypeObject PyFrozenDictIterItem_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "frozendict_itemiterator",                  /* tp_name */
    sizeof(dictiterobject),                     /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)dictiter_dealloc,               /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_as_async */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)dictiter_traverse,            /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    PyObject_SelfIter,                          /* tp_iter */
    (iternextfunc)frozendictiter_iternextitem,  /* tp_iternext */
    dictiter_methods,                           /* tp_methods */
    0,
};

/* The instance lay-out is the same for all three; but the type differs. */

static PyObject *
frozendict_view_new(PyObject *dict, PyTypeObject *type)
{
    _PyDictViewObject *dv;
    if (dict == NULL) {
        PyErr_BadInternalCall();
        return NULL;
    }
    if (!PyAnyDict_Check(dict)) {
        /* XXX Get rid of this restriction later */
        PyErr_Format(PyExc_TypeError,
                     "%s() requires a dict argument, not '%s'",
                     type->tp_name, Py_TYPE(dict)->tp_name);
        return NULL;
    }
    dv = PyObject_GC_New(_PyDictViewObject, type);
    if (dv == NULL)
        return NULL;
    Py_INCREF(dict);
    dv->dv_dict = (PyDictObject *)dict;
    PyObject_GC_Track(dv);
    return (PyObject *)dv;
}

static PyObject *
dictview_new(PyObject *dict, PyTypeObject *type)
{
    _PyDictViewObject *dv;
    if (dict == NULL) {
        PyErr_BadInternalCall();
        return NULL;
    }
    if (!PyAnyDict_Check(dict)) {
        /* XXX Get rid of this restriction later */
        PyErr_Format(PyExc_TypeError,
                     "%s() requires a dict argument, not '%s'",
                     type->tp_name, Py_TYPE(dict)->tp_name);
        return NULL;
    }
    dv = PyObject_GC_New(_PyDictViewObject, type);
    if (dv == NULL)
        return NULL;
    Py_INCREF(dict);
    dv->dv_dict = (PyDictObject *)dict;
    PyObject_GC_Track(dv);
    return (PyObject *)dv;
}

static PyObject *
dictview_mapping(PyObject *view, void *Py_UNUSED(ignored)) {
    assert(view != NULL);
    assert(PyDictKeys_Check(view)
           || PyDictValues_Check(view)
           || PyDictItems_Check(view));
    PyObject *mapping = (PyObject *)((_PyDictViewObject *)view)->dv_dict;
    return PyDictProxy_New(mapping);
}

static PyGetSetDef dictview_getset[] = {
    {"mapping", dictview_mapping, (setter)NULL,
     "dictionary that this view refers to", NULL},
    {0}
};

/*** dict_keys ***/

static PyObject *
frozendictkeys_iter(_PyDictViewObject *dv)
{
    if (dv->dv_dict == NULL) {
        Py_RETURN_NONE;
    }
    return fd_dictiter_new(dv->dv_dict, &PyFrozenDictIterKey_Type);
}

static PyTypeObject PyFrozenDictKeys_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "frozendict_keys",                          /* tp_name */
    sizeof(_PyDictViewObject),                  /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)dictview_dealloc,               /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_as_async */
    (reprfunc)dictview_repr,                    /* tp_repr */
    &dictviews_as_number,                       /* tp_as_number */
    &dictkeys_as_sequence,                      /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)dictview_traverse,            /* tp_traverse */
    0,                                          /* tp_clear */
    dictview_richcompare,                       /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    (getiterfunc)frozendictkeys_iter,                 /* tp_iter */
    0,                                          /* tp_iternext */
    dictkeys_methods,                           /* tp_methods */
    .tp_getset = dictview_getset,
};

static PyObject *
frozendictkeys_new(PyObject *dict, PyObject *Py_UNUSED(ignored))
{
    return dictview_new(dict, &PyFrozenDictKeys_Type);
}

/*** dict_items ***/

static PyObject *
frozendictitems_iter(_PyDictViewObject *dv)
{
    if (dv->dv_dict == NULL) {
        Py_RETURN_NONE;
    }
    return fd_dictiter_new(dv->dv_dict, &PyFrozenDictIterItem_Type);
}

PyTypeObject PyFrozenDictItems_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "frozendict_items",                         /* tp_name */
    sizeof(_PyDictViewObject),                  /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)dictview_dealloc,               /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_as_async */
    (reprfunc)dictview_repr,                    /* tp_repr */
    &dictviews_as_number,                       /* tp_as_number */
    &dictitems_as_sequence,                     /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)dictview_traverse,            /* tp_traverse */
    0,                                          /* tp_clear */
    dictview_richcompare,                       /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    (getiterfunc)frozendictitems_iter,          /* tp_iter */
    0,                                          /* tp_iternext */
    dictitems_methods,                          /* tp_methods */
    .tp_getset = dictview_getset,
};

static PyObject *
frozendictitems_new(PyObject *dict, PyObject *Py_UNUSED(ignored))
{
    return frozendict_view_new(dict, &PyFrozenDictItems_Type);
}

/*** dict_values ***/

static PyObject *
frozendictvalues_iter(_PyDictViewObject *dv)
{
    if (dv->dv_dict == NULL) {
        Py_RETURN_NONE;
    }
    return fd_dictiter_new(dv->dv_dict, &PyFrozenDictIterValue_Type);
}

PyTypeObject PyFrozenDictValues_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "frozendict_values",                        /* tp_name */
    sizeof(_PyDictViewObject),                  /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)dictview_dealloc,               /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_as_async */
    (reprfunc)dictview_repr,                    /* tp_repr */
    0,                                          /* tp_as_number */
    &dictvalues_as_sequence,                    /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)dictview_traverse,            /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    (getiterfunc)frozendictvalues_iter,         /* tp_iter */
    0,                                          /* tp_iternext */
    dictvalues_methods,                         /* tp_methods */
    .tp_getset = dictview_getset,
};

static PyObject *
frozendictvalues_new(PyObject *dict, PyObject *Py_UNUSED(ignored))
{
    return frozendict_view_new(dict, &PyFrozenDictValues_Type);
}

static int
frozendict_exec(PyObject *m)
{
    /* Finalize the type object including setting type of the new type
     * object; doing it here is required for portability, too. */
    if (PyType_Ready(&PyFrozenDict_Type) < 0)
        goto fail;

    if (PyType_Ready(&PyCoold_Type) < 0)
        goto fail;
    
    PyModule_AddObject(m, "frozendict", (PyObject *)&PyFrozenDict_Type);
    PyModule_AddObject(m, "coold", (PyObject *)&PyCoold_Type);
    return 0;
 fail:
    Py_XDECREF(m);
    return -1;
}

static struct PyModuleDef_Slot frozendict_slots[] = {
    {Py_mod_exec, frozendict_exec},
    {0, NULL},
};

static struct PyModuleDef frozendictmodule = {
    PyModuleDef_HEAD_INIT,
    "frozendict",   /* name of module */
    NULL, /* module documentation, may be NULL */
    0,       /* size of per-interpreter state of the module,
                 or -1 if the module keeps state in global variables. */
    NULL,
    frozendict_slots,
    NULL,
    NULL,
    NULL
};

PyMODINIT_FUNC
PyInit__frozendict(void)
{
    return PyModuleDef_Init(&frozendictmodule);
}

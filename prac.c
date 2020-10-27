/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Yonatan Goldschmidt
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <Python.h>
#include <frameobject.h>
#include <interpreteridobject.h>

#include <stdbool.h>
#include <dlfcn.h>

PyDoc_STRVAR(prac__doc__,
"Automatic runtime type checking");

#define SENTINEL ((void*)1)

static size_t extra_idx;

PyFunctionObject *get_function_for_code(PyCodeObject *code) {
    static PyObject *(*gc_get_referrers)(PyObject *self, PyObject *args);
    if (gc_get_referrers == NULL) {
        PyObject *gc = PyImport_ImportModule("gc"); // TODO: really import, see test.py
        if (gc) {
            PyObject *cfunc = PyObject_GetAttrString(gc, "get_referrers");
            if (cfunc && PyCFunction_CheckExact(cfunc)) {
                gc_get_referrers = (typeof(gc_get_referrers))((PyCFunctionObject*)cfunc)->m_ml->ml_meth;
            }

            Py_XDECREF(cfunc);
            Py_DECREF(gc);
        }

        if (NULL == gc_get_referrers) {
            printf("couldn't find gc.get_referrers?\n");
            abort();
        }
    }

    PyObject *tup = PyTuple_Pack(1, code);
    if (NULL == tup) {
        PyErr_Clear();
        return NULL;
    }

    PyObject *referrers = gc_get_referrers(NULL, tup);
    Py_DECREF(tup);

    if (NULL == referrers) {
        PyErr_Clear();
        return NULL;
    }
    assert(PyList_CheckExact(referrers));

    PyObject *func = NULL;
    Py_ssize_t i = 0;
    for (; i < PyList_Size(referrers); i++) {
        PyObject *ref = (PyObject*)PyList_GetItem(referrers, i);

        if (!PyFunction_Check(ref)) {
            continue;
        }

        func = ref;
        Py_INCREF(func);
        break;
    }

    Py_DECREF(referrers);
    return (PyFunctionObject*)func;
}

static bool do_type_checking(PyFunctionObject *func, PyCodeObject *code, PyFrameObject *f) {
    if (func->func_annotations == NULL) {
        return false;
    }

    bool ret = false;

    PyObject *items = PyMapping_Items(func->func_annotations);
    if (items == NULL) {
        goto error;
    }

    PyObject *it = PyObject_GetIter(items);
    Py_DECREF(items);
    if (it == NULL) {
        goto error;
    }

    PyObject *item;
    while ((item = PyIter_Next(it)) != NULL) {
        assert(PyTuple_Check(item) && PyTuple_GET_SIZE(item) == 2 && PyUnicode_CheckExact(PyTuple_GET_ITEM(item, 0)));

        PyObject *key = PyTuple_GET_ITEM(item, 0);
        Py_ssize_t varname_idx = 0;
        for (; varname_idx < PyTuple_GET_SIZE(code->co_varnames); varname_idx++) {
            if (1 == PyObject_RichCompareBool(PyTuple_GET_ITEM(code->co_varnames, varname_idx), key, Py_EQ)) {
                break;
            }
        }

        assert(varname_idx != PyTuple_GET_SIZE(code->co_varnames));
        PyObject *arg = f->f_localsplus[varname_idx];

        // TODO:
        // 1. support postponed evaluation of annotations (from __future__ import annotations)
        // 2. support complex types
        if (!PyType_CheckExact(PyTuple_GET_ITEM(item, 1))) {
            printf("type expected (for now): '%s'\n", Py_TYPE(PyTuple_GET_ITEM(item, 1))->tp_name);
            abort();
        }
        PyTypeObject *expected = (PyTypeObject*)PyTuple_GET_ITEM(item, 1);

        if (!Py_IS_TYPE(arg, expected)) {
            PyErr_Format(PyExc_TypeError, "PRAC: expected type '%s', got '%s' for parameter '%s'",
                expected->tp_name, Py_TYPE(arg)->tp_name, PyUnicode_AsUTF8(key));
            ret = true;
            Py_DECREF(item);
            break;
        }

        Py_DECREF(item);
    }

    Py_DECREF(it);
    // don't PyErr_Clear() in this flow
    return ret;

error:
    PyErr_Clear();
    return ret;
}

PyObject *prac_eval_frame(PyThreadState *tstate, PyFrameObject *f, int exc) {
    void *extra;
    if (_PyCode_GetExtra((PyObject*)f->f_code, extra_idx, &extra) != 0) {
        goto eval_frame;
    }

    PyFunctionObject *func;
    if (extra == NULL) {
        func = get_function_for_code(f->f_code);
        _PyCode_SetExtra((PyObject*)f->f_code, extra_idx, (void*)func ?: SENTINEL);
        if (!func) {
            goto eval_frame;
        }
    } else if (extra == SENTINEL) {
        goto eval_frame;
    } else {
        func = extra;
    }

    if (do_type_checking(func, (PyCodeObject*)f->f_code, f)) {
        return NULL;
    }

eval_frame:
    return _PyEval_EvalFrameDefault(tstate, f, exc);
}

void prac_code_freefunc(void *extra) {
    if (extra != NULL && extra != SENTINEL) {
        Py_DECREF(extra);
    }
}

static PyObject *prac_enable(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    ssize_t _extra_idx = _PyEval_RequestCodeExtraIndex(prac_code_freefunc);
    if (_extra_idx < 0) {
        PyErr_Format(PyExc_ValueError, "used all code extras!");
        return NULL;
    }
    extra_idx = _extra_idx;

    PyInterpreterState *interp = PyInterpreterState_Head();

    while (interp != NULL) {
        assert(_PyInterpreterState_GetEvalFrameFunc(interp) == _PyEval_EvalFrameDefault);
        _PyInterpreterState_SetEvalFrameFunc(interp, prac_eval_frame);
        interp = PyInterpreterState_Next(interp);
    }

    Py_RETURN_NONE;
}


static PyMethodDef prac_methods[] = {
    { "enable", prac_enable, METH_NOARGS, NULL },
    {0, NULL},
};

static struct PyModuleDef pracmodule = {
    PyModuleDef_HEAD_INIT,
    "prac",
    prac__doc__,
    sizeof(pracmodule),
    prac_methods,
    NULL,
    NULL,
    NULL,
    NULL,
};

PyMODINIT_FUNC PyInit_prac(void) {
    return PyModuleDef_Init(&pracmodule);
}

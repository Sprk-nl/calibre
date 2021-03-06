/*
 * cocoa_wrapper.c
 * Copyright (C) 2019 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#include <Python.h>

extern double cocoa_cursor_blink_time(void);
extern void cocoa_send_notification(const char *identitifer, const char *title, const char *subtitle, const char *informativeText, const char* path_to_image);
extern const char* cocoa_send2trash(const char *utf8_path);
extern void activate_cocoa_multithreading(void);

static PyObject *notification_activated_callback = NULL;

static PyObject*
cursor_blink_time(PyObject *self) {
    (void)self;
    double ans = cocoa_cursor_blink_time();
    return PyFloat_FromDouble(ans);
}

void
macos_notification_callback(const char* user_id) {
	if (notification_activated_callback) {
		PyObject *ret = PyObject_CallFunction(notification_activated_callback, "z", user_id);
		if (ret == NULL) PyErr_Print();
		else Py_DECREF(ret);
	}
}

static PyObject*
set_notification_activated_callback(PyObject *self, PyObject *callback) {
    (void)self;
    if (notification_activated_callback) Py_DECREF(notification_activated_callback);
    notification_activated_callback = callback;
    Py_INCREF(callback);
    Py_RETURN_NONE;

}

static PyObject*
send_notification(PyObject *self, PyObject *args) {
	(void)self;
    char *identifier = NULL, *title = NULL, *subtitle = NULL, *informativeText = NULL, *path_to_image = NULL;
    if (!PyArg_ParseTuple(args, "zsz|zz", &identifier, &title, &informativeText, &path_to_image, &subtitle)) return NULL;
	cocoa_send_notification(identifier, title, subtitle, informativeText, path_to_image);

    Py_RETURN_NONE;
}

static PyObject*
send2trash(PyObject *self, PyObject *args) {
	(void)self;
	char *path = NULL;
    if (!PyArg_ParseTuple(args, "s", &path)) return NULL;
	const char *err = cocoa_send2trash(path);
	if (err) {
		PyErr_SetString(PyExc_OSError, err);
		free((void*)err);
		return NULL;
	}
	Py_RETURN_NONE;
}

static PyObject*
enable_cocoa_multithreading(PyObject *self, PyObject *args) {
	activate_cocoa_multithreading();
	Py_RETURN_NONE;
}

static PyMethodDef module_methods[] = {
    {"cursor_blink_time", (PyCFunction)cursor_blink_time, METH_NOARGS, ""},
    {"enable_cocoa_multithreading", (PyCFunction)enable_cocoa_multithreading, METH_NOARGS, ""},
    {"set_notification_activated_callback", (PyCFunction)set_notification_activated_callback, METH_O, ""},
    {"send_notification", (PyCFunction)send_notification, METH_VARARGS, ""},
    {"send2trash", (PyCFunction)send2trash, METH_VARARGS, ""},
    {NULL, NULL, 0, NULL}        /* Sentinel */
};


#if PY_MAJOR_VERSION >= 3
#define INITERROR return NULL
#define INITMODULE PyModule_Create(&bzzdec_module)
static struct PyModuleDef cocoa_module = {
    /* m_base     */ PyModuleDef_HEAD_INIT,
    /* m_name     */ "cocoa",
    /* m_doc      */ "",
    /* m_size     */ -1,
    /* m_methods  */ module_methods,
    /* m_slots    */ 0,
    /* m_traverse */ 0,
    /* m_clear    */ 0,
    /* m_free     */ 0,
};
CALIBRE_MODINIT_FUNC PyInit_cocoa(void) {
#else
#define INITERROR return
#define INITMODULE Py_InitModule3("cocoa", module_methods, "")
CALIBRE_MODINIT_FUNC initcocoa(void) {
#endif

    PyObject *m = INITMODULE;
    if (m == NULL) {
        INITERROR;
    }
#if PY_MAJOR_VERSION >= 3
    return m;
#endif
}

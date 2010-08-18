#include "socket.h"

inline void 
setup_sock(int fd)
{
    int on = 1, r;
    r = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    assert(r == 0);
    r = fcntl(fd, F_SETFL, O_NONBLOCK);
    assert(r == 0);
}

inline void 
enable_cork(client_t *client)
{
    int on = 1, r;
    r = setsockopt(client->fd, IPPROTO_TCP, TCP_CORK, &on, sizeof(on));
    assert(r == 0);
}

inline void 
disable_cork(client_t *client)
{
    int off = 0;
    int on = 1, r;
    r = setsockopt(client->fd, IPPROTO_TCP, TCP_CORK, &off, sizeof(off));
    assert(r == 0);

    r = setsockopt(client->fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    assert(r == 0);
}


inline int 
CheckNSocketObject(PyObject *obj)
{
    if (obj->ob_type != &NSocketObjectType){
        return 0;
    }
    return 1;
}

inline PyObject* 
NSocketObject_New(int fd, ClientObject *client)
{
    NSocketObject *o = PyObject_NEW(NSocketObject, &NSocketObjectType);
    if(o == NULL){
        return NULL;
    }
    o->fd = fd;
    o->client = client;
    Py_INCREF(o->client);    
    setup_sock(fd);
    return (PyObject *)o;
}

static inline void
NSocketObject_dealloc(NSocketObject* self)
{
    Py_DECREF(self->client);
    PyObject_DEL(self);
}

static inline void
send_inner(picoev_loop* loop, int fd, int events, void* cb_arg)
{
    PyObject *obj;
    NSocketObject *socket = (NSocketObject *)cb_arg;
    buffer *send_buf = socket->send_buf;
    
    if ((events & PICOEV_TIMEOUT) != 0) {
        //
        PyErr_SetString(PyExc_IOError, "write timeout");
        
        switch_wsgi_app(loop, socket->fd, (PyObject *)socket->client);
    } else if ((events & PICOEV_WRITE) != 0) {
        ssize_t r;
        r = write(socket->fd, send_buf->buf, send_buf->len);
        switch (r) {
            case -1:
                if (errno == EAGAIN || errno == EWOULDBLOCK) { /* try again later */
                    break;
                } else { /* fatal error */
                    PyErr_SetFromErrno(PyExc_IOError);
                    switch_wsgi_app(loop, socket->fd, (PyObject *)socket->client);
                    return;
                }
                break;
            default:
                send_buf->buf += r;
                send_buf->len -= r;
                if(!send_buf->len){
                    //all done
                    //switch 
                    obj = Py_BuildValue("(i)", send_buf->buf_size);
                    socket->client->args = obj;
                    switch_wsgi_app(loop, socket->fd, (PyObject *)socket->client);
                }
                break;
        }
    }
}


static inline void
recv_inner(picoev_loop* loop, int fd, int events, void* cb_arg)
{
    PyObject *obj;

    NSocketObject *socket = (NSocketObject *)cb_arg;
    buffer *recv_buf = socket->recv_buf;
    
    if ((events & PICOEV_TIMEOUT) != 0) {

        free_buffer(socket->recv_buf);
        PyErr_SetString(PyExc_IOError, "read timeout");
        
        switch_wsgi_app(loop, socket->fd, (PyObject *)socket->client);
    
    } else if ((events & PICOEV_READ) != 0) {
        //printf("read \n");

        ssize_t r;
        r = read(socket->fd, recv_buf->buf, recv_buf->len);
        // update timeout
        picoev_set_timeout(loop, socket->fd, 5);
        switch (r) {
            case -1:
                if (errno == EAGAIN || errno == EWOULDBLOCK) { /* try again later */
                    break;
                } else { /* fatal error */
                    free_buffer(socket->recv_buf);
                    PyErr_SetFromErrno(PyExc_IOError);
                    switch_wsgi_app(loop, socket->fd, (PyObject *)socket->client);
                    return;
                }
                break;
            default:
                recv_buf->buf += r;
                recv_buf->len -= r;
                if(!recv_buf->len){
                    //all done
                    //switch 
                    obj = Py_BuildValue("(O)", getPyString(socket->recv_buf));
                    socket->client->args = obj;
                    switch_wsgi_app(loop, socket->fd, (PyObject *)socket->client);
                }
                break;
        }
    }
}

static inline PyObject * 
recv_ready(NSocketObject *socket, ssize_t len)
{
    PyGreenlet *current, *parent;

    socket->recv_buf = new_buffer(len, len);
    picoev_add(main_loop, socket->fd, PICOEV_READ, 5, recv_inner, (void *)socket);
    
    // switch to hub
    current = socket->client->greenlet;
    parent = PyGreenlet_GET_PARENT(current);
    return PyGreenlet_Switch(parent, hub_switch_value, NULL);
}

static inline PyObject * 
send_ready(NSocketObject *socket, char *buf, ssize_t len)
{
    PyGreenlet *current, *parent;

    socket->send_buf = new_buffer(len, len);
    socket->send_buf->buf = buf;
    socket->send_buf->len = len;

    picoev_add(main_loop, socket->fd, PICOEV_WRITE, 0, send_inner, (void *)socket);
    
    // switch to hub
    current = socket->client->greenlet;
    parent = PyGreenlet_GET_PARENT(current);
    return PyGreenlet_Switch(parent, hub_switch_value, NULL);
}

static inline PyObject * 
NSocketObject_recv(NSocketObject *socket, PyObject *args)
{
    ssize_t len; 
    if (!PyArg_ParseTuple(args, "i:recv", &len)){
        return NULL;
    }
    return recv_ready(socket, len);
}

static inline PyObject * 
NSocketObject_send(NSocketObject *socket, PyObject *args)
{
    PyObject *s;
    char *buf;
    ssize_t len;
    if (!PyArg_ParseTuple(args, "S:send", &s)){
        return NULL;
    }
    
    PyString_AsStringAndSize(s, &buf, &len);
    printf("NSocketObject_send \n");

    return send_ready(socket, buf, len);
}

static PyMethodDef NSocketObject_method[] = {
    { "recv",      (PyCFunction)NSocketObject_recv, METH_VARARGS, 0 },
    { "send",      (PyCFunction)NSocketObject_send, METH_VARARGS, 0 },
    { NULL, NULL}
};



PyTypeObject NSocketObjectType = {
	PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "meinheld.nsocket",             /*tp_name*/
    sizeof(NSocketObject), /*tp_basicsize*/
    0,                         /*tp_itemsize*/
    (destructor)NSocketObject_dealloc, /*tp_dealloc*/
    0,                         /*tp_print*/
    0,                         /*tp_getattr*/
    0,                         /*tp_setattr*/
    0,                         /*tp_compare*/
    0,                         /*tp_repr*/
    0,                         /*tp_as_number*/
    0,                         /*tp_as_sequence*/
    0,                         /*tp_as_mapping*/
    0,                         /*tp_hash */
    0,                         /*tp_call*/
    0,                         /*tp_str*/
    0,                         /*tp_getattro*/
    0,                         /*tp_setattro*/
    0,                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT,        /*tp_flags*/
    "io raw socket ",                 /* tp_doc */
    0,		               /* tp_traverse */
    0,		               /* tp_clear */
    0,		               /* tp_richcompare */
    0,		               /* tp_weaklistoffset */
    0,		               /* tp_iter */
    0,		                   /* tp_iternext */
    NSocketObject_method,        /* tp_methods */
    0,                         /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    0,                      /* tp_init */
    0,                         /* tp_alloc */
    0,                           /* tp_new */
};

inline void 
setup_nsocket(void)
{
    PyGreenlet_Import();
}


#include <stdlib.h>
#include "couch_common.h"
#include "util.h"

#define ERR_MIN -7

const char* errordescs[7] =
{
      "error opening file"        // ERROR_OPEN_FILE
    , "error reading erlang term" // ERROR_PARSE_TERM
    , "failed to allocate buffer" // ERROR_ALLOC_FAIL
    , "error reading file"        // ERROR_READ
    , "document not found"        // DOC_NOT_FOUND
    , "no header in non-empty file" // ERROR_NO_HEADER
    , "error writing to file" // ERROR_WRITE
};

const char* describe_error(int errcode)
{
    if(errcode < 0 && errcode >= ERR_MIN)
        return errordescs[-1-errcode];
    else
        return NULL;
}

void term_to_buf(sized_buf *dst, char* buf, int *pos)
{
    int start = *pos;
    ei_skip_term(buf, pos);
    dst->buf = buf + start;
    dst->size = *pos - start;
}

node_pointer* read_root(char* buf, int* endpos)
{
    //Parse {ptr, reduce_value, subtreesize} into a node_pointer with no key.
    node_pointer* ptr;
    int size, type;
    int pos = *endpos;
    ei_get_type(buf, &pos, &type, &size);
    ei_skip_term(buf, endpos);
    if(type == ERL_ATOM_EXT)
        return NULL;
    size = *endpos - pos;
    //Copy the erlang term into the buffer.
    ptr = malloc(sizeof(node_pointer) + size);
    buf = memcpy(((char*)ptr) + sizeof(node_pointer), buf + pos, size);
    pos = 0;
    ptr->key.buf = NULL;
    ptr->key.size = 0;
    ei_decode_tuple_header(buf, &pos, NULL); //arity 3
    ei_decode_ulonglong(buf, &pos, (unsigned long long*) &ptr->pointer);
    term_to_buf(&ptr->reduce_value, buf, &pos);
    ei_decode_ulonglong(buf, &pos, (unsigned long long*) &ptr->subtreesize);
    return ptr;
}

void ei_x_encode_nodepointer(ei_x_buff* x, node_pointer* node)
{
    if(node == NULL)
    {
        ei_x_encode_atom(x, "nil");
    }
    else
    {
        ei_x_encode_tuple_header(x, 3);
        ei_x_encode_ulonglong(x, node->pointer);
        ei_x_append_buf(x, node->reduce_value.buf, node->reduce_value.size);
        ei_x_encode_ulonglong(x, node->subtreesize);
    }
}
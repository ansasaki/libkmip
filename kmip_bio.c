/* Copyright (c) 2018 The Johns Hopkins University/Applied Physics Laboratory
 * All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#include <openssl/ssl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "kmip.h"
#include "kmip_memset.h"

/*
OpenSSH BIO API
*/

int kmip_bio_create(BIO *bio, int max_message_size,
                    struct template_attribute *template_attribute,
                    char **id, size_t *id_size)
{
    /* Set up the KMIP context and the initial encoding buffer. */
    struct kmip ctx = {0};
    kmip_init(&ctx, NULL, 0, KMIP_1_0);
    
    size_t buffer_blocks = 1;
    size_t buffer_block_size = 1024;
    size_t buffer_total_size = buffer_blocks * buffer_block_size;
    
    uint8 *encoding = ctx.calloc_func(ctx.state, buffer_blocks,
                                      buffer_block_size);
    if(encoding == NULL)
    {
        kmip_destroy(&ctx);
        return(KMIP_MEMORY_ALLOC_FAILED);
    }
    kmip_set_buffer(&ctx, encoding, buffer_total_size);
    
    /* Build the request message. */
    struct protocol_version pv = {0};
    init_protocol_version(&pv, ctx.version);
    
    struct request_header rh = {0};
    init_request_header(&rh);
    
    rh.protocol_version = &pv;
    rh.maximum_response_size = max_message_size;
    rh.time_stamp = time(NULL);
    rh.batch_count = 1;
    
    struct create_request_payload crp = {0};
    crp.object_type = KMIP_OBJTYPE_SYMMETRIC_KEY;
    crp.template_attribute = template_attribute;
    
    struct request_batch_item rbi = {0};
    rbi.operation = KMIP_OP_CREATE;
    rbi.request_payload = &crp;
    
    struct request_message rm = {0};
    rm.request_header = &rh;
    rm.batch_items = &rbi;
    rm.batch_count = 1;
    
    /* Encode the request message. Dynamically resize the encoding buffer */
    /* if it's not big enough. Once encoding succeeds, send the request   */
    /* message.                                                           */
    int encode_result = encode_request_message(&ctx, &rm);
    while(encode_result == KMIP_ERROR_BUFFER_FULL)
    {
        kmip_reset(&ctx);
        ctx.free_func(ctx.state, encoding);
        
        buffer_blocks += 1;
        buffer_total_size = buffer_blocks * buffer_block_size;
        
        encoding = ctx.calloc_func(ctx.state, buffer_blocks,
                                   buffer_block_size);
        if(encoding == NULL)
        {
            kmip_destroy(&ctx);
            return(KMIP_MEMORY_ALLOC_FAILED);
        }
        
        kmip_set_buffer(
            &ctx,
            encoding,
            buffer_total_size);
        encode_result = encode_request_message(&ctx, &rm);
    }
    
    if(encode_result != KMIP_OK)
    {
        free_buffer(&ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_destroy(&ctx);
        return(encode_result);
    }
    
    int sent = BIO_write(bio, ctx.buffer, ctx.index - ctx.buffer);
    if(sent != ctx.index - ctx.buffer)
    {
        free_buffer(&ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_destroy(&ctx);
        return(KMIP_IO_FAILURE);
    }
    
    free_buffer(&ctx, encoding, buffer_total_size);
    encoding = NULL;
    
    /* Read the response message. Dynamically resize the encoding buffer  */
    /* to align with the message size advertised by the message encoding. */
    /* Reject the message if the message size is too large.               */
    buffer_blocks = 1;
    buffer_block_size = 8;
    buffer_total_size = buffer_blocks * buffer_block_size;
    
    encoding = ctx.calloc_func(ctx.state, buffer_blocks, buffer_block_size);
    if(encoding == NULL)
    {
        kmip_destroy(&ctx);
        return(KMIP_MEMORY_ALLOC_FAILED);
    }
    
    int recv = BIO_read(bio, encoding, buffer_total_size);
    if((size_t)recv != buffer_total_size)
    {
        free_buffer(&ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_destroy(&ctx);
        return(KMIP_IO_FAILURE);
    }
    
    kmip_set_buffer(&ctx, encoding, buffer_total_size);
    ctx.index += 4;
    int length = 0;
    
    decode_int32_be(&ctx, &length);
    kmip_rewind(&ctx);
    if(length > max_message_size)
    {
        free_buffer(&ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_destroy(&ctx);
        return(KMIP_EXCEED_MAX_MESSAGE_SIZE);
    }
    
    uint8 *extended = ctx.realloc_func(ctx.state, encoding, buffer_total_size + length);
    if(encoding != extended)
    {
        encoding = extended;
    }
    ctx.memset_func(encoding + buffer_total_size, 0, length);
    
    buffer_block_size += length;
    buffer_total_size = buffer_blocks * buffer_block_size;
    
    recv = BIO_read(bio, encoding + 8, length);
    if(recv != length)
    {
        free_buffer(&ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_destroy(&ctx);
        return(KMIP_IO_FAILURE);
    }
    
    kmip_reset(&ctx);
    kmip_set_buffer(&ctx, encoding, buffer_block_size);
    
    /* Decode the response message and retrieve the operation results. */
    struct response_message resp_m = {0};
    int decode_result = decode_response_message(&ctx, &resp_m);
    if(decode_result != KMIP_OK)
    {
        free_buffer(&ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_destroy(&ctx);
        return(decode_result);
    }
    
    enum result_status result = KMIP_STATUS_OPERATION_FAILED;
    if(resp_m.batch_count != 1 || resp_m.batch_items == NULL)
    {
        free_buffer(&ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_destroy(&ctx);
        return(KMIP_MALFORMED_RESPONSE);
    }
    
    struct response_batch_item resp_item = resp_m.batch_items[0];
    result = resp_item.result_status;
    
    struct create_response_payload *pld = 
        (struct create_response_payload *)resp_item.response_payload;
    struct text_string *unique_identifier = pld->unique_identifier;
    
    char *result_id = ctx.calloc_func(ctx.state, 1, unique_identifier->size);
    *id_size = unique_identifier->size;
    for(size_t i = 0; i < *id_size; i++)
    {
        result_id[i] = unique_identifier->value[i];
    }
    *id = result_id;
    
    /* Clean up the response message, the encoding buffer, and the KMIP */
    /* context. */
    free_response_message(&ctx, &resp_m);
    free_buffer(&ctx, encoding, buffer_total_size);
    encoding = NULL;
    kmip_set_buffer(&ctx, NULL, 0);
    kmip_destroy(&ctx);
    
    return(result);
}

int kmip_bio_destroy(BIO *bio, int max_message_size, char *uuid,
                     size_t uuid_size)
{
    /* Set up the KMIP context and the initial encoding buffer. */
    struct kmip ctx = {0};
    kmip_init(&ctx, NULL, 0, KMIP_1_0);
    
    size_t buffer_blocks = 1;
    size_t buffer_block_size = 1024;
    size_t buffer_total_size = buffer_blocks * buffer_block_size;
    
    uint8 *encoding = ctx.calloc_func(ctx.state, buffer_blocks,
                                      buffer_block_size);
    if(encoding == NULL)
    {
        kmip_destroy(&ctx);
        return(KMIP_MEMORY_ALLOC_FAILED);
    }
    kmip_set_buffer(&ctx, encoding, buffer_total_size);
    
    /* Build the request message. */
    struct protocol_version pv = {0};
    init_protocol_version(&pv, ctx.version);
    
    struct request_header rh = {0};
    init_request_header(&rh);
    
    rh.protocol_version = &pv;
    rh.maximum_response_size = max_message_size;
    rh.time_stamp = time(NULL);
    rh.batch_count = 1;
    
    struct text_string id = {0};
    id.value = uuid;
    id.size = uuid_size;
    
    struct destroy_request_payload drp = {0};
    drp.unique_identifier = &id;
    
    struct request_batch_item rbi = {0};
    rbi.operation = KMIP_OP_DESTROY;
    rbi.request_payload = &drp;
    
    struct request_message rm = {0};
    rm.request_header = &rh;
    rm.batch_items = &rbi;
    rm.batch_count = 1;
    
    /* Encode the request message. Dynamically resize the encoding buffer */
    /* if it's not big enough. Once encoding succeeds, send the request   */
    /* message.                                                           */
    int encode_result = encode_request_message(&ctx, &rm);
    while(encode_result == KMIP_ERROR_BUFFER_FULL)
    {
        kmip_reset(&ctx);
        ctx.free_func(ctx.state, encoding);
        
        buffer_blocks += 1;
        buffer_total_size = buffer_blocks * buffer_block_size;
        
        encoding = ctx.calloc_func(ctx.state, buffer_blocks,
                                   buffer_block_size);
        if(encoding == NULL)
        {
            kmip_destroy(&ctx);
            return(KMIP_MEMORY_ALLOC_FAILED);
        }
        
        kmip_set_buffer(
            &ctx,
            encoding,
            buffer_total_size);
        encode_result = encode_request_message(&ctx, &rm);
    }
    
    if(encode_result != KMIP_OK)
    {
        free_buffer(&ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_destroy(&ctx);
        return(encode_result);
    }
    
    int sent = BIO_write(bio, ctx.buffer, ctx.index - ctx.buffer);
    if(sent != ctx.index - ctx.buffer)
    {
        free_buffer(&ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_destroy(&ctx);
        return(KMIP_IO_FAILURE);
    }
    
    free_buffer(&ctx, encoding, buffer_total_size);
    encoding = NULL;
    
    /* Read the response message. Dynamically resize the encoding buffer  */
    /* to align with the message size advertised by the message encoding. */
    /* Reject the message if the message size is too large.               */
    buffer_blocks = 1;
    buffer_block_size = 8;
    buffer_total_size = buffer_blocks * buffer_block_size;
    
    encoding = ctx.calloc_func(ctx.state, buffer_blocks, buffer_block_size);
    if(encoding == NULL)
    {
        kmip_destroy(&ctx);
        return(KMIP_MEMORY_ALLOC_FAILED);
    }
    
    int recv = BIO_read(bio, encoding, buffer_total_size);
    if((size_t)recv != buffer_total_size)
    {
        free_buffer(&ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_destroy(&ctx);
        return(KMIP_IO_FAILURE);
    }
    
    kmip_set_buffer(&ctx, encoding, buffer_total_size);
    ctx.index += 4;
    int length = 0;
    
    decode_int32_be(&ctx, &length);
    kmip_rewind(&ctx);
    if(length > max_message_size)
    {
        free_buffer(&ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_destroy(&ctx);
        return(KMIP_EXCEED_MAX_MESSAGE_SIZE);
    }
    
    uint8 *extended = ctx.realloc_func(ctx.state, encoding, buffer_total_size + length);
    if(encoding != extended)
    {
        encoding = extended;
    }
    ctx.memset_func(encoding + buffer_total_size, 0, length);
    
    buffer_block_size += length;
    buffer_total_size = buffer_blocks * buffer_block_size;
    
    recv = BIO_read(bio, encoding + 8, length);
    if(recv != length)
    {
        free_buffer(&ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_destroy(&ctx);
        return(KMIP_IO_FAILURE);
    }
    
    kmip_reset(&ctx);
    kmip_set_buffer(&ctx, encoding, buffer_block_size);
    
    /* Decode the response message and retrieve the operation result status. */
    struct response_message resp_m = {0};
    int decode_result = decode_response_message(&ctx, &resp_m);
    if(decode_result != KMIP_OK)
    {
        free_buffer(&ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_destroy(&ctx);
        return(decode_result);
    }
    
    enum result_status result = KMIP_STATUS_OPERATION_FAILED;
    if(resp_m.batch_count != 1 || resp_m.batch_items == NULL)
    {
        free_buffer(&ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_destroy(&ctx);
        return(KMIP_MALFORMED_RESPONSE);
    }
    
    struct response_batch_item resp_item = resp_m.batch_items[0];
    result = resp_item.result_status;
    
    /* Clean up the response message, the encoding buffer, and the KMIP */
    /* context. */
    free_response_message(&ctx, &resp_m);
    free_buffer(&ctx, encoding, buffer_total_size);
    encoding = NULL;
    kmip_set_buffer(&ctx, NULL, 0);
    kmip_destroy(&ctx);
    
    return(result);
}

int kmip_bio_get_symmetric_key(BIO *bio, int max_message_size, char *id,
                               size_t id_size, char **key, size_t *key_size)
{
    /* Set up the KMIP context and the initial encoding buffer. */
    struct kmip ctx = {0};
    kmip_init(&ctx, NULL, 0, KMIP_1_0);
    
    size_t buffer_blocks = 1;
    size_t buffer_block_size = 1024;
    size_t buffer_total_size = buffer_blocks * buffer_block_size;
    
    uint8 *encoding = ctx.calloc_func(ctx.state, buffer_blocks,
                                      buffer_block_size);
    if(encoding == NULL)
    {
        kmip_destroy(&ctx);
        return(KMIP_MEMORY_ALLOC_FAILED);
    }
    kmip_set_buffer(&ctx, encoding, buffer_total_size);
    
    /* Build the request message. */
    struct protocol_version pv = {0};
    init_protocol_version(&pv, ctx.version);
    
    struct request_header rh = {0};
    init_request_header(&rh);
    
    rh.protocol_version = &pv;
    rh.maximum_response_size = max_message_size;
    rh.time_stamp = time(NULL);
    rh.batch_count = 1;
    
    struct text_string uuid = {0};
    uuid.value = id;
    uuid.size = id_size;
    
    struct get_request_payload grp = {0};
    grp.unique_identifier = &uuid;
    
    struct request_batch_item rbi = {0};
    rbi.operation = KMIP_OP_GET;
    rbi.request_payload = &grp;
    
    struct request_message rm = {0};
    rm.request_header = &rh;
    rm.batch_items = &rbi;
    rm.batch_count = 1;
    
    /* Encode the request message. Dynamically resize the encoding buffer */
    /* if it's not big enough. Once encoding succeeds, send the request   */
    /* message.                                                           */
    int encode_result = encode_request_message(&ctx, &rm);
    while(encode_result == KMIP_ERROR_BUFFER_FULL)
    {
        kmip_reset(&ctx);
        ctx.free_func(ctx.state, encoding);
        
        buffer_blocks += 1;
        buffer_total_size = buffer_blocks * buffer_block_size;
        
        encoding = ctx.calloc_func(ctx.state, buffer_blocks,
                                   buffer_block_size);
        if(encoding == NULL)
        {
            kmip_destroy(&ctx);
            return(KMIP_MEMORY_ALLOC_FAILED);
        }
        
        kmip_set_buffer(
            &ctx,
            encoding,
            buffer_total_size);
        encode_result = encode_request_message(&ctx, &rm);
    }
    
    if(encode_result != KMIP_OK)
    {
        free_buffer(&ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_destroy(&ctx);
        return(encode_result);
    }
    
    int sent = BIO_write(bio, ctx.buffer, ctx.index - ctx.buffer);
    if(sent != ctx.index - ctx.buffer)
    {
        free_buffer(&ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_destroy(&ctx);
        return(KMIP_IO_FAILURE);
    }
    
    free_buffer(&ctx, encoding, buffer_total_size);
    encoding = NULL;
    
    /* Read the response message. Dynamically resize the encoding buffer  */
    /* to align with the message size advertised by the message encoding. */
    /* Reject the message if the message size is too large.               */
    buffer_blocks = 1;
    buffer_block_size = 8;
    buffer_total_size = buffer_blocks * buffer_block_size;
    
    encoding = ctx.calloc_func(ctx.state, buffer_blocks, buffer_block_size);
    if(encoding == NULL)
    {
        kmip_destroy(&ctx);
        return(KMIP_MEMORY_ALLOC_FAILED);
    }
    
    int recv = BIO_read(bio, encoding, buffer_total_size);
    if((size_t)recv != buffer_total_size)
    {
        free_buffer(&ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_destroy(&ctx);
        return(KMIP_IO_FAILURE);
    }
    
    kmip_set_buffer(&ctx, encoding, buffer_total_size);
    ctx.index += 4;
    int length = 0;
    
    decode_int32_be(&ctx, &length);
    kmip_rewind(&ctx);
    if(length > max_message_size)
    {
        free_buffer(&ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_destroy(&ctx);
        return(KMIP_EXCEED_MAX_MESSAGE_SIZE);
    }
    
    uint8 *extended = ctx.realloc_func(ctx.state, encoding, buffer_total_size + length);
    if(encoding != extended)
    {
        encoding = extended;
    }
    ctx.memset_func(encoding + buffer_total_size, 0, length);
    
    buffer_block_size += length;
    buffer_total_size = buffer_blocks * buffer_block_size;
    
    recv = BIO_read(bio, encoding + 8, length);
    if(recv != length)
    {
        free_buffer(&ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_destroy(&ctx);
        return(KMIP_IO_FAILURE);
    }
    
    kmip_reset(&ctx);
    kmip_set_buffer(&ctx, encoding, buffer_block_size);
    
    /* Decode the response message and retrieve the operation result status. */
    struct response_message resp_m = {0};
    int decode_result = decode_response_message(&ctx, &resp_m);
    if(decode_result != KMIP_OK)
    {
        free_buffer(&ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_destroy(&ctx);
        return(decode_result);
    }
    
    free_buffer(&ctx, encoding, buffer_total_size);
    encoding = NULL;
    
    enum result_status result = KMIP_STATUS_OPERATION_FAILED;
    if(resp_m.batch_count != 1 || resp_m.batch_items == NULL)
    {
        free_response_message(&ctx, &resp_m);
        kmip_set_buffer(&ctx, NULL, 0);
        kmip_destroy(&ctx);
        return(KMIP_MALFORMED_RESPONSE);
    }
    
    struct response_batch_item resp_item = resp_m.batch_items[0];
    result = resp_item.result_status;
    
    if(result != KMIP_STATUS_SUCCESS)
    {
        free_response_message(&ctx, &resp_m);
        kmip_set_buffer(&ctx, NULL, 0);
        kmip_destroy(&ctx);
        return(result);
    }
    
    struct get_response_payload *pld = 
        (struct get_response_payload *)resp_item.response_payload;
    
    if(pld->object_type != KMIP_OBJTYPE_SYMMETRIC_KEY)
    {
        free_response_message(&ctx, &resp_m);
        kmip_set_buffer(&ctx, NULL, 0);
        kmip_destroy(&ctx);
        return(KMIP_OBJECT_MISMATCH);
    }
    
    struct symmetric_key *symmetric_key = (struct symmetric_key *)pld->object;
    struct key_block *block = symmetric_key->key_block;
    if((block->key_format_type != KMIP_KEYFORMAT_RAW) || 
       (block->key_wrapping_data != NULL))
    {
        free_response_message(&ctx, &resp_m);
        kmip_set_buffer(&ctx, NULL, 0);
        kmip_destroy(&ctx);
        return(KMIP_OBJECT_MISMATCH);
    }
    
    struct key_value *block_value = block->key_value;
    struct byte_string *material = (struct byte_string *)block_value->key_material;
    
    char *result_key = ctx.calloc_func(ctx.state, 1, material->size);
    *key_size = material->size;
    for(size_t i = 0; i < *key_size; i++)
    {
        result_key[i] = material->value[i];
    }
    *key = result_key;
    
    /* Clean up the response message, the encoding buffer, and the KMIP */
    /* context. */
    free_response_message(&ctx, &resp_m);
    free_buffer(&ctx, encoding, buffer_total_size);
    encoding = NULL;
    kmip_set_buffer(&ctx, NULL, 0);
    kmip_destroy(&ctx);
    
    return(result);
}

int kmip_bio_create_with_context(
struct kmip *ctx,
BIO *bio,
int max_message_size,
struct template_attribute *template_attribute,
char **id,
size_t *id_size)
{
    /* Set up the initial encoding buffer. */
    size_t buffer_blocks = 1;
    size_t buffer_block_size = 1024;
    size_t buffer_total_size = buffer_blocks * buffer_block_size;
    
    uint8 *encoding = ctx->calloc_func(ctx->state, buffer_blocks,
                                       buffer_block_size);
    if(encoding == NULL)
    {
        return(KMIP_MEMORY_ALLOC_FAILED);
    }
    kmip_set_buffer(ctx, encoding, buffer_total_size);
    
    /* Build the request message. */
    struct protocol_version pv = {0};
    init_protocol_version(&pv, ctx->version);
    
    struct request_header rh = {0};
    init_request_header(&rh);
    
    rh.protocol_version = &pv;
    rh.maximum_response_size = max_message_size;
    rh.time_stamp = time(NULL);
    rh.batch_count = 1;
    
    struct create_request_payload crp = {0};
    crp.object_type = KMIP_OBJTYPE_SYMMETRIC_KEY;
    crp.template_attribute = template_attribute;
    
    struct request_batch_item rbi = {0};
    rbi.operation = KMIP_OP_CREATE;
    rbi.request_payload = &crp;
    
    struct request_message rm = {0};
    rm.request_header = &rh;
    rm.batch_items = &rbi;
    rm.batch_count = 1;
    
    /* Encode the request message. Dynamically resize the encoding buffer */
    /* if it's not big enough. Once encoding succeeds, send the request   */
    /* message.                                                           */
    int encode_result = encode_request_message(ctx, &rm);
    while(encode_result == KMIP_ERROR_BUFFER_FULL)
    {
        kmip_reset(ctx);
        ctx->free_func(ctx->state, encoding);
        
        buffer_blocks += 1;
        buffer_total_size = buffer_blocks * buffer_block_size;
        
        encoding = ctx->calloc_func(ctx->state, buffer_blocks,
                                    buffer_block_size);
        if(encoding == NULL)
        {
            kmip_set_buffer(ctx, NULL, 0);
            return(KMIP_MEMORY_ALLOC_FAILED);
        }
        
        kmip_set_buffer(
            ctx,
            encoding,
            buffer_total_size);
        encode_result = encode_request_message(ctx, &rm);
    }
    
    if(encode_result != KMIP_OK)
    {
        free_buffer(ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_set_buffer(ctx, NULL, 0);
        return(encode_result);
    }
    
    int sent = BIO_write(bio, ctx->buffer, ctx->index - ctx->buffer);
    if(sent != ctx->index - ctx->buffer)
    {
        free_buffer(ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_set_buffer(ctx, NULL, 0);
        return(KMIP_IO_FAILURE);
    }
    
    free_buffer(ctx, encoding, buffer_total_size);
    encoding = NULL;
    kmip_set_buffer(ctx, NULL, 0);
    
    /* Read the response message. Dynamically resize the encoding buffer  */
    /* to align with the message size advertised by the message encoding. */
    /* Reject the message if the message size is too large.               */
    buffer_blocks = 1;
    buffer_block_size = 8;
    buffer_total_size = buffer_blocks * buffer_block_size;
    
    encoding = ctx->calloc_func(ctx->state, buffer_blocks, buffer_block_size);
    if(encoding == NULL)
    {
        return(KMIP_MEMORY_ALLOC_FAILED);
    }
    
    int recv = BIO_read(bio, encoding, buffer_total_size);
    if((size_t)recv != buffer_total_size)
    {
        free_buffer(ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_set_buffer(ctx, NULL, 0);
        return(KMIP_IO_FAILURE);
    }
    
    kmip_set_buffer(ctx, encoding, buffer_total_size);
    ctx->index += 4;
    int length = 0;
    
    decode_int32_be(ctx, &length);
    kmip_rewind(ctx);
    if(length > max_message_size)
    {
        free_buffer(ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_set_buffer(ctx, NULL, 0);
        return(KMIP_EXCEED_MAX_MESSAGE_SIZE);
    }
    
    uint8 *extended = ctx->realloc_func(ctx->state, encoding, buffer_total_size + length);
    if(encoding != extended)
    {
        encoding = extended;
    }
    ctx->memset_func(encoding + buffer_total_size, 0, length);
    
    buffer_block_size += length;
    buffer_total_size = buffer_blocks * buffer_block_size;
    
    recv = BIO_read(bio, encoding + 8, length);
    if(recv != length)
    {
        free_buffer(ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_set_buffer(ctx, NULL, 0);
        return(KMIP_IO_FAILURE);
    }
    
    kmip_reset(ctx);
    kmip_set_buffer(ctx, encoding, buffer_block_size);
    
    /* Decode the response message and retrieve the operation results. */
    struct response_message resp_m = {0};
    int decode_result = decode_response_message(ctx, &resp_m);
    
    kmip_set_buffer(ctx, NULL, 0);
    
    if(decode_result != KMIP_OK)
    {
        free_buffer(ctx, encoding, buffer_total_size);
        encoding = NULL;
        return(decode_result);
    }
    
    enum result_status result = KMIP_STATUS_OPERATION_FAILED;
    if(resp_m.batch_count != 1 || resp_m.batch_items == NULL)
    {
        free_buffer(ctx, encoding, buffer_total_size);
        encoding = NULL;
        return(KMIP_MALFORMED_RESPONSE);
    }
    
    struct response_batch_item resp_item = resp_m.batch_items[0];
    result = resp_item.result_status;
    
    struct create_response_payload *pld = 
        (struct create_response_payload *)resp_item.response_payload;
    struct text_string *unique_identifier = pld->unique_identifier;
    
    char *result_id = ctx->calloc_func(ctx->state, 1, unique_identifier->size);
    *id_size = unique_identifier->size;
    for(size_t i = 0; i < *id_size; i++)
    {
        result_id[i] = unique_identifier->value[i];
    }
    *id = result_id;
    
    /* Clean up the response message and the encoding buffer. */
    free_response_message(ctx, &resp_m);
    free_buffer(ctx, encoding, buffer_total_size);
    encoding = NULL;
    kmip_set_buffer(ctx, NULL, 0);
    
    return(result);
}

/*
int kmip_bio_destroy_with_context(struct kmip *ctx, BIO *bio, char *uuid, size_t uuid_size,
 int max_msg_size)
{
return(KMIP_OK);
}

int kmip_bio_get_symmetric_key_with_context(
struct kmip *ctx,
BIO *bio,
char *uuid,
size_t uuid_size,
char **key,
size_t *key_size,
 int max_msg_size)
{
return(KMIP_OK);
}

int kmip_bio_send_request(
BIO *bio,
struct request_message *request,
struct response_message **response,
size_t max_msg_size)
{
return(KMIP_OK);
}

int kmip_bio_send_request_with_context(
struct kmip *ctx,
BIO *bio,
struct request_message *request, 
struct response_message **response,
size_t max_msg_size)
{
return(KMIP_OK);
}

int kmip_bio_send_request_encoding(
BIO *bio,
char *request,
size_t request_size,
char **response,
size_t *response_size,
size_t max_msg_size)
{
return(KMIP_OK);
}
*/
int kmip_bio_send_request_encoding(
struct kmip *ctx,
BIO *bio,
int max_message_size,
char *request,
size_t request_size,
char **response,
size_t *response_size)
{
    /* Send the request message. */
    int sent = BIO_write(bio, request, request_size);
    if((size_t)sent != request_size)
    {
        return(KMIP_IO_FAILURE);
    }
    
    /* Read the response message. Dynamically resize the receiving buffer */
    /* to align with the message size advertised by the message encoding. */
    /* Reject the message if the message size is too large.               */
    size_t buffer_blocks = 1;
    size_t buffer_block_size = 8;
    size_t buffer_total_size = buffer_blocks * buffer_block_size;
    
    uint8 *encoding = ctx->calloc_func(ctx->state, buffer_blocks,
                                       buffer_block_size);
    if(encoding == NULL)
    {
        return(KMIP_MEMORY_ALLOC_FAILED);
    }
    
    int recv = BIO_read(bio, encoding, buffer_total_size);
    if((size_t)recv != buffer_total_size)
    {
        free_buffer(ctx, encoding, buffer_total_size);
        encoding = NULL;
        return(KMIP_IO_FAILURE);
    }
    
    kmip_set_buffer(ctx, encoding, buffer_total_size);
    ctx->index += 4;
    int length = 0;
    
    decode_int32_be(ctx, &length);
    kmip_rewind(ctx);
    if(length > max_message_size)
    {
        free_buffer(ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_set_buffer(ctx, NULL, 0);
        return(KMIP_EXCEED_MAX_MESSAGE_SIZE);
    }
    
    uint8 *extended = ctx->realloc_func(ctx->state, encoding,
                                        buffer_total_size + length);
    if(encoding != extended)
    {
        encoding = extended;
    }
    ctx->memset_func(encoding + buffer_total_size, 0, length);
    
    buffer_block_size += length;
    buffer_total_size = buffer_blocks * buffer_block_size;
    
    recv = BIO_read(bio, encoding + 8, length);
    if(recv != length)
    {
        free_buffer(ctx, encoding, buffer_total_size);
        encoding = NULL;
        kmip_set_buffer(ctx, NULL, 0);
        return(KMIP_IO_FAILURE);
    }
    
    *response_size = buffer_total_size;
    *response = (char *)encoding;
    
    kmip_set_buffer(ctx, NULL, 0);
    
    return(KMIP_OK);
}
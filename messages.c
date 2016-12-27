/*
 * Copyright 2016 Iaroslav Zeigerman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "messages.h"
#include "network.h"


#define RETURN_IF_INVALID_PAYLOAD(t, r) if (!message_is_payload_valid(buffer, buffer_size, (t))) return r;

static uint16_t uint16_decode(const uint8_t *buffer) {
    return PT_NTOHS(*(uint16_t *) buffer);
}

static void uint16_encode(uint16_t n, uint8_t *buffer) {
    uint16_t network_n = PT_HTONS(n);
    memcpy(buffer, &network_n, sizeof(uint16_t));
}

static uint32_t uint32_decode(const uint8_t *buffer) {
    return PT_NTOHL(*(uint32_t *) buffer);
}

static void uint32_encode(uint32_t n, uint8_t *buffer) {
    uint32_t network_n = PT_HTONL(n);
    memcpy(buffer, &network_n, sizeof(uint32_t));
}

static int cluster_member_decode(const uint8_t *buffer, cluster_member_t *member) {
    const uint8_t *cursor = buffer;
    member->version = uint16_decode(cursor);
    cursor += sizeof(uint16_t);
    member->uid = uint32_decode(cursor);
    cursor += sizeof(uint32_t);
    member->address_len = uint32_decode(cursor);
    cursor += sizeof(uint32_t);
    member->address = (pt_sockaddr_storage *) cursor;
    cursor += member->address_len;
    return cursor - buffer;
}

static int cluster_member_encode(const cluster_member_t *member, uint8_t *buffer) {
    uint8_t *cursor = buffer;
    uint16_encode(member->version, cursor);
    cursor += sizeof(uint16_t);
    uint32_encode(member->uid, cursor);
    cursor += sizeof(uint32_t);
    uint32_encode(member->address_len, cursor);
    cursor += sizeof(uint32_t);
    memcpy(cursor, member->address, member->address_len);
    cursor += member->address_len;
    return cursor - buffer;
}

int message_type_decode(const uint8_t *buffer, size_t buffer_size) {
    if (buffer_size < sizeof(message_header_t)) return -1;
    return *(buffer + PROTOCOL_ID_LENGTH);
}

static int message_is_payload_valid(const uint8_t *buffer, size_t buffer_size, uint8_t type) {
    return message_type_decode(buffer, buffer_size) == type &&
            memcmp(buffer, PROTOCOL_ID, PROTOCOL_ID_LENGTH) == 0;
}

void message_header_create(message_header_t *header, uint8_t message_type) {
    memcpy(header->protocol_id, PROTOCOL_ID, PROTOCOL_ID_LENGTH);
    header->message_type = message_type;
}

static int message_header_encode(const message_header_t *msg, uint8_t *buffer, size_t buffer_size) {
    if (buffer_size < sizeof(struct message_header)) return -1;
    memcpy(buffer, msg->protocol_id, PROTOCOL_ID_LENGTH);
    *(buffer + PROTOCOL_ID_LENGTH) = msg->message_type;
    return sizeof(struct message_header);
}

static int message_header_decode(const uint8_t *buffer, size_t buffer_size, message_header_t *result) {
    if (buffer_size < sizeof(struct message_header)) return -1;
    memcpy(result->protocol_id, buffer, PROTOCOL_ID_LENGTH);
    result->message_type = *(buffer + PROTOCOL_ID_LENGTH);
    return sizeof(struct message_header);
}

int message_hello_decode(const uint8_t *buffer, size_t buffer_size, message_hello_t *result) {
    RETURN_IF_INVALID_PAYLOAD(MESSAGE_HELLO_TYPE, -1);
    size_t min_size = sizeof(message_header_t) + sizeof(cluster_member_t) - sizeof(pt_sockaddr_storage *);
    if (buffer_size < min_size) return -1;

    message_header_decode(buffer, buffer_size, &result->header);
    result->this_member = (cluster_member_t *) malloc(sizeof(cluster_member_t));
    int member_bytes = cluster_member_decode(buffer + sizeof(message_header_t), result->this_member);
    return sizeof(message_header_t) + member_bytes;
}

int message_hello_encode(const message_hello_t *msg, uint8_t *buffer, size_t buffer_size) {
    size_t expected_size = sizeof(message_header_t) + sizeof(cluster_member_t) -
            sizeof(pt_sockaddr_storage *) + msg->this_member->address_len;
    if (buffer_size < expected_size) return -1;

    int encode_result = message_header_encode(&msg->header, buffer, buffer_size);
    if (encode_result < 0) return -1;

    uint8_t *cursor = buffer + encode_result;
    cursor += cluster_member_encode(msg->this_member, cursor);

    return cursor - buffer;
}

void message_hello_destroy(const message_hello_t *msg) {
    free(msg->this_member);
}

int message_welcome_decode(const uint8_t *buffer, size_t buffer_size, message_welcome_t *result) {
    RETURN_IF_INVALID_PAYLOAD(MESSAGE_WELCOME_TYPE, -1);
    message_header_decode(buffer, buffer_size, &result->header);
    return sizeof(message_welcome_t);
}

int message_welcome_encode(const message_welcome_t *msg, uint8_t *buffer, size_t buffer_size) {
    if (buffer_size < sizeof(message_header_t)) return -1;
    return message_header_encode(&msg->header, buffer, buffer_size);
}

int message_data_decode(const uint8_t *buffer, size_t buffer_size, message_data_t *result) {
    RETURN_IF_INVALID_PAYLOAD(MESSAGE_DATA_TYPE, -1);

    const uint8_t *cursor = buffer;

    if (message_header_decode(cursor, buffer_size, &result->header) < 0) return -1;
    cursor += sizeof(message_header_t);

    result->data_size = uint32_decode(cursor);
    cursor += sizeof(uint32_t);

    size_t base_size = sizeof(message_header_t) + sizeof(uint32_t);
    size_t expected_size = base_size + result->data_size;
    if (buffer_size != expected_size) return -1;

    uint8_t *data_cursor = result->data_size > 0 ? (uint8_t *) cursor : NULL;
    result->data = data_cursor;

    return (cursor + result->data_size) - buffer;
}

int message_data_encode(const message_data_t *msg, uint8_t *buffer, size_t buffer_size) {
    if (buffer_size < sizeof(message_header_t) + sizeof(uint32_t) + msg->data_size) return -1;

    int encode_result = message_header_encode(&msg->header, buffer, buffer_size);
    if (encode_result < 0) return -1;

    uint8_t *cursor = buffer + encode_result;

    uint32_encode(msg->data_size, cursor);
    cursor += sizeof(uint32_t);

    memcpy(cursor, msg->data, msg->data_size);
    cursor += msg->data_size;

    return cursor - buffer;
}

int message_member_list_decode(const uint8_t *buffer, size_t buffer_size, message_member_list_t *result) {
    RETURN_IF_INVALID_PAYLOAD(MESSAGE_MEMBER_LIST_TYPE, -1);

    const uint8_t *cursor = buffer;

    if (message_header_decode(cursor, buffer_size, &result->header) < 0) return -1;
    cursor += sizeof(message_header_t);

    result->members_n = uint16_decode(cursor);
    cursor += sizeof(uint16_t);

    result->members = (cluster_member_t *) malloc(result->members_n * sizeof(cluster_member_t));

    for (int i = 0; i < result->members_n; ++i) {
        cursor += cluster_member_decode(cursor, &result->members[i]);
    }

    return cursor - buffer;
}

int message_member_list_encode(const message_member_list_t *msg, uint8_t *buffer, size_t buffer_size) {
    uint32_t expected_size = sizeof(message_header_t) + sizeof(uint16_t);
    expected_size += msg->members_n * (sizeof(uint16_t) + sizeof(uint32_t) + sizeof(pt_sockaddr_storage));
    if (buffer_size < expected_size) return -1;

    int encode_result = message_header_encode(&msg->header, buffer, buffer_size);
    if (encode_result < 0) return -1;

    uint8_t *cursor = buffer + encode_result;
    uint16_encode(msg->members_n, cursor);
    cursor += sizeof(uint16_t);

    for (int i = 0; i < msg->members_n; ++i) {
        cursor += cluster_member_encode(&msg->members[i], cursor);
    }
    return cursor - buffer;
}

void message_member_list_destroy(const message_member_list_t *msg) {
    free(msg->members);
}

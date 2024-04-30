#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <esp_log.h>
#include <cstring>

#include "rbwifi.h"

#include "rbdns.h"

#define TAG "RbDns"

#define OPCODE_MASK (0x7800)
#define QR_FLAG (1 << 7)
#define QD_TYPE_A (0x0001)
#define ANS_TTL_SEC (300)

static const ssize_t DNS_MSG_SIZE_LIMIT = 256;

typedef struct __attribute__((__packed__)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

typedef struct __attribute__((__packed__)) {
    uint16_t type;
    uint16_t clazz;
} dns_question_t;

typedef struct __attribute__((__packed__)) {
    uint16_t ptr_offset;
    uint16_t type;
    uint16_t clazz;
    uint32_t ttl;
    uint16_t addr_len;
    uint32_t ip_addr;
} dns_answer_t;

namespace rb {

DnsServer& DnsServer::get() {
    static DnsServer instance;
    return instance;
}

DnsServer::DnsServer()
    : m_socket(-1)
    , m_task(nullptr) {
}

DnsServer::~DnsServer() {
}

void DnsServer::start(const char* local_hostname) {
    const std::lock_guard<std::mutex> l(m_mutex);
    if (m_task != nullptr) {
        ESP_LOGE(TAG, "DnsServer::start called when it was already started, doing nothing.");
        return;
    }

    m_local_hostname = local_hostname;

    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == -1) {
        ESP_LOGE(TAG, "failed to create socket: %s", strerror(errno));
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(m_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "failed to bind socket: %s", strerror(errno));
        close(m_socket);
        m_socket = -1;
        return;
    }

    xTaskCreate(DnsServer::taskBody, "rbdns", 3072, NULL, 2, &m_task);
}

void DnsServer::stop() {
    const std::lock_guard<std::mutex> l(m_mutex);
    if (m_task == nullptr) {
        ESP_LOGE(TAG, "DnsServer::stop called when it was not started, doing nothing.");
        return;
    }

    close(m_socket);
    m_socket = -1;
    m_task = NULL;
}

ssize_t DnsServer::receivePacket(std::vector<uint8_t>& buff, struct sockaddr_in* addr) {
    ssize_t msg_size;

    while (true) {
        msg_size = recvfrom(m_socket, (void*)buff.data(), buff.size(), MSG_PEEK | MSG_DONTWAIT, NULL, NULL);
        if (msg_size < 0) {
            const auto err = errno;
            if (err == EAGAIN) {
                vTaskDelay(pdMS_TO_TICKS(30));
                continue;
            }

            if (err == EBADF) {
                return -EBADF;
            }

            ESP_LOGE(TAG, "error in recvfrom: %d %s!", err, strerror(err));
            return -1;
        }

        if (msg_size < buff.size() || msg_size > DNS_MSG_SIZE_LIMIT)
            break;
        buff.resize(msg_size);
    }

    socklen_t addr_len = sizeof(struct sockaddr_in);
    const auto pop_res = recvfrom(m_socket, (void*)buff.data(), 0, MSG_DONTWAIT, (struct sockaddr*)addr, &addr_len);
    if (pop_res < 0) {
        const auto err = errno;
        if (err == EBADF)
            return -EBADF;
        ESP_LOGE(TAG, "error in recvfrom: %d %s!", err, strerror(err));
        return -1;
    }

    if (msg_size > DNS_MSG_SIZE_LIMIT) {
        ESP_LOGW(TAG, "Oversize DNS packet received: %d", msg_size);
        return -1;
    }

    return msg_size;
}

uint8_t* DnsServer::parseDnsName(uint8_t* src_data, size_t maxlen, std::string& out_name) {
    char out_buf[64];
    char* out_buf_ptr = out_buf;

    if (maxlen > sizeof(out_buf)) {
        maxlen = sizeof(out_buf);
    }

    uint8_t* label = src_data;
    do {
        const uint8_t label_len = *label;
        if (label_len > maxlen || label_len == 0) {
            ESP_LOGE(TAG, "too long label encountered, %d", label_len);
            return NULL;
        }

        memcpy(out_buf_ptr, label + 1, label_len);

        out_buf_ptr[label_len] = '.';
        out_buf_ptr += label_len + 1;
        maxlen -= label_len + 1;
        label += label_len + 1;
    } while (*label != 0);

    out_name.assign(out_buf, out_buf_ptr - out_buf - 1);

    return label + 1;
}

ssize_t DnsServer::processDnsQuestion(std::vector<uint8_t>& buff, ssize_t req_size) {
    dns_header_t* header = (dns_header_t*)buff.data();

    ESP_LOGD(TAG, "DNS query with header id: 0x%X, flags: 0x%X, qd_count: %d",
        ntohs(header->id), ntohs(header->flags), ntohs(header->qd_count));

    // Not a standard query
    if ((ntohs(header->flags) & OPCODE_MASK) != 0) {
        return -1;
    }

    // Set question response flag
    header->flags |= QR_FLAG;
    header->ns_count = 0;
    header->ar_count = 0;

    const uint16_t question_count = ntohs(header->qd_count);
    uint16_t answer_count = 0;

    const ssize_t full_reply_len = question_count * sizeof(dns_answer_t) + req_size;
    if (full_reply_len > DNS_MSG_SIZE_LIMIT) {
        ESP_LOGW(TAG, "DNS packet has too many questions %d %dB, ignoring.", question_count, full_reply_len);
        return -1;
    }

    if (buff.size() < full_reply_len) {
        buff.resize(full_reply_len);
        header = (dns_header_t*)buff.data();
    }

    uint8_t* req_end = buff.data() + req_size;
    uint8_t* cur_question_ptr = buff.data() + sizeof(dns_header_t);
    uint8_t* cur_ans_ptr = req_end;

    const auto cur_esp_ip = WiFi::getIp();

    std::string hostname;
    for (uint16_t question_idx = 0; question_idx < question_count; ++question_idx) {
        uint8_t* name_end_ptr = parseDnsName(cur_question_ptr, req_end - cur_question_ptr, hostname);
        if (!name_end_ptr) {
            return -1;
        }

        dns_question_t* question = (dns_question_t*)(name_end_ptr);
        uint16_t qd_type = ntohs(question->type);
        uint16_t qd_class = ntohs(question->clazz);

        ESP_LOGD(TAG, "Received type: %d | Class: %d | Question for: %s", qd_type, qd_class, hostname.c_str());

        if (qd_type != QD_TYPE_A) {
            cur_question_ptr = name_end_ptr + sizeof(dns_question_t);
            continue;
        }

        dns_answer_t* answer = (dns_answer_t*)cur_ans_ptr;

        answer->ptr_offset = htons(0xC000 | (cur_question_ptr - buff.data()));
        answer->type = htons(qd_type);
        answer->clazz = htons(qd_class);
        answer->ttl = htonl(ANS_TTL_SEC);

        ESP_LOGD(TAG, "Answer with PTR offset: 0x%" PRIX16 " and IP 0x%" PRIX32, ntohs(answer->ptr_offset), cur_esp_ip);

        /*
        TODO: add support for custom DNS entries, now it always returns ESP's IP.
        if(hostname == m_local_hostname) {

        }*/

        answer->addr_len = htons(sizeof(cur_esp_ip));
        answer->ip_addr = cur_esp_ip;

        cur_question_ptr = name_end_ptr + sizeof(dns_question_t);
        cur_ans_ptr += sizeof(dns_answer_t);
        ++answer_count;
    }

    header->an_count = htons(answer_count);

    // Drop any extra RRs, namespace...
    if (cur_question_ptr < req_end) {
        const size_t extra_req_data_len = req_end - cur_question_ptr;
        memmove(cur_question_ptr, req_end, extra_req_data_len);
        req_size -= extra_req_data_len;
    }

    const ssize_t generated_reply_len = answer_count * sizeof(dns_answer_t) + req_size;
    return generated_reply_len;
}

void DnsServer::taskBody(void*) {
    auto& self = get();


    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(struct sockaddr_in);

    ssize_t msg_size;

    {
        // vTaskDelete does not call destructors
        std::vector<uint8_t> buff;
        buff.resize(64);

        while (true) {
            msg_size = self.receivePacket(buff, &addr);
            if (msg_size == -EBADF) {
                break;
            } else if (msg_size < 0) {
                continue;
            }

            // Reset remaining data to 0
            memset(buff.data() + msg_size, 0, buff.size() - msg_size);

            ssize_t reply_size = self.processDnsQuestion(buff, msg_size);
            if (reply_size < 0) {
                continue;
            }

            if (sendto(self.m_socket, buff.data(), reply_size, 0, (struct sockaddr*)&addr, addr_len) < 0) {
                const auto err = errno;
                if (err == EBADF)
                    break;
                ESP_LOGE(TAG, "error in sendto: %d %s, len %d!", err, strerror(err), reply_size);
            }
        }
    }

    vTaskDelete(nullptr);
}

};

extern "C" const char* rb_dn_get_local_hostname() {
    return rb::DnsServer::get().getLocalHostname().c_str();
}

/* SPDX-License-Identifier: LGPL-2.1-or-later */

/* Make sure the net/if.h header is included before any linux/ one */
#include <net/if.h>
#include <linux/if_addrlabel.h>

#include "alloc-util.h"
#include "netlink-util.h"
#include "networkd-address-label.h"
#include "networkd-link.h"
#include "networkd-manager.h"
#include "networkd-network.h"
#include "networkd-queue.h"
#include "parse-util.h"

AddressLabel *address_label_free(AddressLabel *label) {
        if (!label)
                return NULL;

        if (label->network) {
                assert(label->section);
                hashmap_remove(label->network->address_labels_by_section, label->section);
        }

        if (label->manager) {
                assert(label->section);
                hashmap_remove(label->manager->address_labels_by_section, label->section);
        }

        config_section_free(label->section);
        return mfree(label);
}

DEFINE_SECTION_CLEANUP_FUNCTIONS(AddressLabel, address_label_free);

DEFINE_PRIVATE_HASH_OPS_WITH_VALUE_DESTRUCTOR(
        address_label_section_hash_ops,
        ConfigSection,
        config_section_hash_func,
        config_section_compare_func,
        AddressLabel,
        address_label_free);

static int address_label_new_static(
                Manager *manager,
                Network *network,
                const char *filename,
                unsigned section_line,
                AddressLabel **ret) {

        _cleanup_(config_section_freep) ConfigSection *n = NULL;
        _cleanup_(address_label_freep) AddressLabel *label = NULL;
        Hashmap **address_labels_by_section;
        int r;

        assert(!manager != !network);
        assert(ret);
        assert(filename);
        assert(section_line > 0);

        address_labels_by_section = manager ? &manager->address_labels_by_section : &network->address_labels_by_section;

        r = config_section_new(filename, section_line, &n);
        if (r < 0)
                return r;

        label = hashmap_get(*address_labels_by_section, n);
        if (label) {
                *ret = TAKE_PTR(label);
                return 0;
        }

        label = new(AddressLabel, 1);
        if (!label)
                return -ENOMEM;

        *label = (AddressLabel) {
                .manager = manager,
                .network = network,
                .section = TAKE_PTR(n),
                .label = UINT32_MAX,
        };

        r = hashmap_ensure_put(address_labels_by_section, &address_label_section_hash_ops, label->section, label);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(label);
        return 0;
}

static int link_address_label_configure_handler(
                sd_netlink *rtnl,
                sd_netlink_message *m,
                Request *req,
                Link *link,
                void *userdata) {

        int r;

        assert(m);
        assert(link);

        r = sd_netlink_message_get_errno(m);
        if (r < 0 && r != -EEXIST) {
                log_link_message_warning_errno(link, m, r, "Could not set address label");
                link_enter_failed(link);
                return 1;
        }

        if (link->static_address_label_messages == 0) {
                log_link_debug(link, "Addresses label set.");
                link->static_address_labels_configured = true;
                link_check_ready(link);
        }

        return 1;
}

static int manager_address_label_configure_handler(
                sd_netlink *rtnl,
                sd_netlink_message *m,
                Request *req,
                Link *link,
                void *userdata) {

        Manager *manager = ASSERT_PTR(ASSERT_PTR(req)->manager);
        int r;

        assert(m);
        assert(!link);

        r = sd_netlink_message_get_errno(m);
        if (r < 0 && r != -EEXIST) {
                log_message_warning_errno(m, r, "Could not set address label");
                return 1;
        }

        if (manager->static_address_label_messages == 0) {
                log_debug("Addresses label set.");
                manager->static_address_labels_configured = true;
        }

        return 1;
}

static int address_label_fill_message(AddressLabel *label, sd_netlink_message *m) {
        int r;

        assert(label);
        assert(m);

        r = sd_rtnl_message_addrlabel_set_prefixlen(m, label->prefixlen);
        if (r < 0)
                return r;

        r = sd_netlink_message_append_u32(m, IFAL_LABEL, label->label);
        if (r < 0)
                return r;

        return sd_netlink_message_append_in6_addr(m, IFA_ADDRESS, &label->prefix);
}

static int link_address_label_configure(AddressLabel *label, Link *link, Request *req) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *m = NULL;
        int r;

        assert(label);
        assert(link);
        assert(link->ifindex > 0);
        assert(link->manager);
        assert(link->manager->rtnl);
        assert(req);

        r = sd_rtnl_message_new_addrlabel(link->manager->rtnl, &m, RTM_NEWADDRLABEL,
                                          link->ifindex, AF_INET6);
        if (r < 0)
                return r;

        r = address_label_fill_message(label, m);
        if (r < 0)
                return r;

        return request_call_netlink_async(link->manager->rtnl, m, req);
}

static int manager_address_label_configure(AddressLabel *label, Manager *manager, Request *req) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *m = NULL;
        int r;

        assert(label);
        assert(manager);
        assert(manager->rtnl);
        assert(req);

        r = sd_rtnl_message_new_addrlabel(manager->rtnl, &m, RTM_NEWADDRLABEL, 0, AF_INET6);
        if (r < 0)
                return r;

        r = address_label_fill_message(label, m);
        if (r < 0)
                return r;

        return request_call_netlink_async(manager->rtnl, m, req);
}

static int link_address_label_process_request(Request *req, Link *link, void *userdata) {
        AddressLabel *label = ASSERT_PTR(userdata);
        int r;

        assert(req);
        assert(link);
        assert(link->manager);

        if (!link_is_ready_to_configure(link, /* allow_unmanaged = */ false))
                return 0;

        if (!link->manager->static_address_labels_configured)
                return 0;

        r = link_address_label_configure(label, link, req);
        if (r < 0)
                return log_link_warning_errno(link, r, "Failed to configure address label: %m");

        return 1;
}

static int manager_address_label_process_request(Request *req, Link *link, void *userdata) {
        AddressLabel *label = ASSERT_PTR(userdata);
        int r;

        assert(req);
        assert(req->manager);
        assert(!link);

        r = manager_address_label_configure(label, req->manager, req);
        if (r < 0)
                return log_warning_errno(r, "Failed to configure address label: %m");

        return 1;
}

int link_request_static_address_labels(Link *link) {
        AddressLabel *label;
        int r;

        assert(link);
        assert(link->network);

        link->static_address_labels_configured = false;

        HASHMAP_FOREACH(label, link->network->address_labels_by_section) {
                r = link_queue_request_full(link, REQUEST_TYPE_ADDRESS_LABEL,
                                            label, NULL, trivial_hash_func, trivial_compare_func,
                                            link_address_label_process_request,
                                            &link->static_address_label_messages,
                                            link_address_label_configure_handler, NULL);
                if (r < 0)
                        return log_link_warning_errno(link, r, "Failed to request address label: %m");
        }

        if (link->static_address_label_messages == 0) {
                link->static_address_labels_configured = true;
                link_check_ready(link);
        } else {
                log_link_debug(link, "Setting address labels.");
                link_set_state(link, LINK_STATE_CONFIGURING);
        }

        return 0;
}

int manager_request_static_address_labels(Manager *manager) {
        AddressLabel *label;
        int r;

        assert(manager);

        manager->static_address_labels_configured = false;

        HASHMAP_FOREACH(label, manager->address_labels_by_section) {
                r = manager_queue_request_full(manager, REQUEST_TYPE_ADDRESS_LABEL,
                                               label, NULL, trivial_hash_func, trivial_compare_func,
                                               manager_address_label_process_request,
                                               &manager->static_address_label_messages,
                                               manager_address_label_configure_handler, NULL);
                if (r < 0)
                        return log_warning_errno(r, "Failed to request address label: %m");
        }

        if (manager->static_address_label_messages == 0)
                manager->static_address_labels_configured = true;
        else
                log_debug("Setting address labels.");

        return 0;
}

static int address_label_section_verify(AddressLabel *label) {
        assert(label);
        assert(label->section);

        if (section_is_invalid(label->section))
                return -EINVAL;

        if (!label->prefix_set)
                return log_warning_errno(SYNTHETIC_ERRNO(EINVAL),
                                         "%s: [IPv6AddressLabel] section without Prefix= setting specified. "
                                         "Ignoring [IPv6AddressLabel] section from line %u.",
                                         label->section->filename, label->section->line);

        if (label->label == UINT32_MAX)
                return log_warning_errno(SYNTHETIC_ERRNO(EINVAL),
                                         "%s: [IPv6AddressLabel] section without Label= setting specified. "
                                         "Ignoring [IPv6AddressLabel] section from line %u.",
                                         label->section->filename, label->section->line);

        return 0;
}

static void drop_invalid_address_labels(Hashmap *address_labels_by_section) {
        AddressLabel *label;

        HASHMAP_FOREACH(label, address_labels_by_section)
                if (address_label_section_verify(label) < 0)
                        address_label_free(label);
}

void network_drop_invalid_address_labels(Network *network) {
        assert(network);
        drop_invalid_address_labels(network->address_labels_by_section);
}

void manager_drop_invalid_address_labels(Manager *manager) {
        assert(manager);
        drop_invalid_address_labels(manager->address_labels_by_section);
}

int config_parse_address_label_prefix(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        _cleanup_(address_label_free_or_set_invalidp) AddressLabel *n = NULL;
        Manager *manager = ltype ? userdata : NULL;
        Network *network = ltype ? NULL : userdata;
        unsigned char prefixlen;
        union in_addr_union a;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(userdata);

        r = address_label_new_static(manager, network, filename, section_line, &n);
        if (r < 0)
                return log_oom();

        if (isempty(rvalue)) {
                n->prefix_set = false;
                TAKE_PTR(n);
                return 0;
        }

        r = in_addr_prefix_from_string(rvalue, AF_INET6, &a, &prefixlen);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Invalid prefix for address label, ignoring assignment: %s", rvalue);
                return 0;
        }
        if (in6_addr_is_ipv4_mapped_address(&a.in6) && prefixlen > 96) {
                /* See ip6addrlbl_alloc() in net/ipv6/addrlabel.c of kernel. */
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "The prefix length of IPv4 mapped address for address label must be equal to or smaller than 96, "
                           "ignoring assignment: %s", rvalue);
                return 0;
        }

        n->prefix = a.in6;
        n->prefixlen = prefixlen;
        n->prefix_set = true;
        TAKE_PTR(n);
        return 0;
}

int config_parse_address_label(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        _cleanup_(address_label_free_or_set_invalidp) AddressLabel *n = NULL;
        Manager *manager = ltype ? userdata : NULL;
        Network *network = ltype ? NULL : userdata;
        uint32_t k;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(userdata);

        r = address_label_new_static(manager, network, filename, section_line, &n);
        if (r < 0)
                return log_oom();

        if (isempty(rvalue)) {
                n->label = UINT32_MAX;
                TAKE_PTR(n);
                return 0;
        }

        r = safe_atou32(rvalue, &k);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r, "Failed to parse address label, ignoring: %s", rvalue);
                return 0;
        }

        if (k == UINT32_MAX) {
                log_syntax(unit, LOG_WARNING, filename, line, 0, "Address label is invalid, ignoring: %s", rvalue);
                return 0;
        }

        n->label = k;
        TAKE_PTR(n);
        return 0;
}

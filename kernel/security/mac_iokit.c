/*
 * Copyright (c) 2007 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*-
 * Copyright (c) 2006 SPARTA, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/queue.h>
#include <bsd/bsm/audit.h>
#include <bsd/security/audit/audit.h>
#include <bsd/sys/malloc.h>
#include <vm/vm_kern.h>

#if DEVELOPMENT || DEBUG
#include <pexpert/pexpert.h>
#include <sys/proc.h>
#include <sys/systm.h>
#endif

#include <security/mac_framework.h>
#include <security/mac_internal.h>

#if (DEVELOPMENT || DEBUG) && !SECURE_KERNEL
static boolean_t mac_iokit_development_open_allowed(const char *, unsigned int);
#endif

int
mac_iokit_check_open_service(kauth_cred_t cred, io_object_t service, unsigned int user_client_type)
{
	return mac_iokit_check_open_service_development(cred, service, user_client_type, NULL);
}

int
mac_iokit_check_open_service_development(kauth_cred_t cred, io_object_t service, unsigned int user_client_type,
    const char *service_name)
{
	int error;
	(void)service_name;

#if DEVELOPMENT || DEBUG
	int trace = 0;
	boolean_t development_allow = FALSE;
#if !SECURE_KERNEL
	if (service_name && strcmp(service_name, "QcomGPU") == 0) {
		const char *client = (user_client_type & 0xffff) == 1 ? "QcomGPUDeviceUserClient" :
		    (user_client_type & 0xffff) == 33 ? "IOGPUMemoryInfoUserClient" : NULL;
		development_allow = mac_iokit_development_open_allowed(client, user_client_type);
	}
#endif
	PE_parse_boot_argn("iokit-wait-trace", &trace, sizeof(trace));
	if (trace || development_allow) {
		error = 0;
		MAC_POLICY_ITERATE({
			if (mpc->mpc_ops->mpo_iokit_check_open_service &&
			    !(development_allow && strcmp(mpc->mpc_name, "Sandbox") == 0)) {
				MAC_CHECK_CALL(iokit_check_open_service, mpc);
				int __step_err = mpc->mpc_ops->mpo_iokit_check_open_service(cred, service, user_client_type);
				MAC_CHECK_RSLT(iokit_check_open_service, mpc);
				if (__step_err) printf("iokit denial: pid=%d policy=%s hook=iokit_check_open_service type=%u errno=%d\n",
				    proc_selfpid(), mpc->mpc_name, user_client_type, __step_err);
				error = mac_error_select(__step_err, error);
			}
		});
	} else
#endif
	{
	MAC_CHECK(iokit_check_open_service, cred, service, user_client_type);
	}
	return error;
}

#if (DEVELOPMENT || DEBUG) && !SECURE_KERNEL
static boolean_t
mac_iokit_development_open_allowed(const char *client_name, unsigned int type)
{
	char allowed[MAXPATHLEN], path[MAXPATHLEN];
	int size = sizeof(path);
	struct vnode *vp;
	boolean_t display_client = client_name && type == 0 &&
	    (strcmp(client_name, "IOMobileFramebufferUserClient") == 0 ||
	    strcmp(client_name, "RootDomainUserClient") == 0 ||
	    strcmp(client_name, "IOSurfaceRootUserClient") == 0);
	/* Native Family connection types; vendor task/entitlement checks still run. */
	boolean_t gpu_client = client_name &&
	    ((strcmp(client_name, "QcomGPUDeviceUserClient") == 0 && (type & 0xffff) == 1) ||
	    (strcmp(client_name, "IOGPUMemoryInfoUserClient") == 0 && (type & 0xffff) == 33));
	if ((!display_client && !gpu_client) ||
	    !PE_i_can_has_debugger(NULL) ||
	    !PE_parse_boot_argn("development-sandbox-iokit-path", allowed, sizeof(allowed)) ||
	    allowed[0] != '/') {
		return FALSE;
	}
	allowed[sizeof(allowed) - 1] = '\0';
	vp = proc_getexecutablevnode(current_proc());
	if (!vp) return FALSE;
	int error = vn_getpath(vp, path, &size);
	vnode_put(vp);
	if (error) return FALSE;
	for (char *entry = allowed, *next; entry && *entry; entry = next) {
		next = strchr(entry, ':');
		if (next) *next++ = '\0';
		if (entry[0] == '/' && strcmp(path, entry) == 0) {
			printf("iokit development allowance: Sandbox path=%s client=%s type=%u\n",
			    path, client_name, type);
			return TRUE;
		}
	}
	if (gpu_client) printf("iokit development GPU path not listed: pid=%d path=%s client=%s type=%u\n",
	    proc_selfpid(), path, client_name, type);
	return FALSE;
}
#endif

int
mac_iokit_check_open(kauth_cred_t cred, io_object_t user_client, unsigned int user_client_type)
{
	return mac_iokit_check_open_development(cred, user_client, user_client_type, NULL, NULL);
}

int
mac_iokit_check_open_development(kauth_cred_t cred, io_object_t user_client, unsigned int user_client_type,
    const char *client_name, boolean_t *development_allowed)
{
	int error;
	(void)client_name;
	if (development_allowed) *development_allowed = FALSE;

#if DEVELOPMENT || DEBUG
	int trace = 0;
	boolean_t development_allow = FALSE;
#if !SECURE_KERNEL
	development_allow = mac_iokit_development_open_allowed(client_name, user_client_type);
	if (development_allowed) *development_allowed = development_allow;
#endif
	PE_parse_boot_argn("iokit-wait-trace", &trace, sizeof(trace));
	if (trace || development_allow) {
		error = 0;
		MAC_POLICY_ITERATE({
			if (mpc->mpc_ops->mpo_iokit_check_open &&
			    !(development_allow && strcmp(mpc->mpc_name, "Sandbox") == 0)) {
				MAC_CHECK_CALL(iokit_check_open, mpc);
				int __step_err = mpc->mpc_ops->mpo_iokit_check_open(cred, user_client, user_client_type);
				MAC_CHECK_RSLT(iokit_check_open, mpc);
				if (__step_err) printf("iokit denial: pid=%d policy=%s hook=iokit_check_open type=%u errno=%d\n",
				    proc_selfpid(), mpc->mpc_name, user_client_type, __step_err);
				error = mac_error_select(__step_err, error);
			}
		});
	} else
#endif
	{
	MAC_CHECK(iokit_check_open, cred, user_client, user_client_type);
	}
	return error;
}

int
mac_iokit_check_set_properties(kauth_cred_t cred, io_object_t registry_entry, io_object_t properties)
{
	int error;

	MAC_CHECK(iokit_check_set_properties, cred, registry_entry, properties);
	return error;
}

int
mac_iokit_check_filter_properties(kauth_cred_t cred, io_object_t registry_entry)
{
	int error;

	MAC_CHECK(iokit_check_filter_properties, cred, registry_entry);
	return error;
}

int
mac_iokit_check_get_property(kauth_cred_t cred, io_object_t registry_entry, const char *name)
{
	int error;

	MAC_CHECK(iokit_check_get_property, cred, registry_entry, name);
	return error;
}

int
mac_iokit_check_hid_control(kauth_cred_t cred)
{
	int error;

	MAC_CHECK(iokit_check_hid_control, cred);
	return error;
}

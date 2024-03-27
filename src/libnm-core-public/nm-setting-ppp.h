/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2007 - 2008 Red Hat, Inc.
 * Copyright (C) 2007 - 2008 Novell, Inc.
 */

#ifndef __NM_SETTING_PPP_H__
#define __NM_SETTING_PPP_H__

#if !defined(__NETWORKMANAGER_H_INSIDE__) && !defined(NETWORKMANAGER_COMPILATION)
#error "Only <NetworkManager.h> can be included directly."
#endif

#include "nm-setting.h"

G_BEGIN_DECLS

#define NM_TYPE_SETTING_PPP (nm_setting_ppp_get_type())
#define NM_SETTING_PPP(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), NM_TYPE_SETTING_PPP, NMSettingPpp))
#define NM_SETTING_PPP_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST((klass), NM_TYPE_SETTING_PPP, NMSettingPppClass))
#define NM_IS_SETTING_PPP(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj), NM_TYPE_SETTING_PPP))
#define NM_IS_SETTING_PPP_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), NM_TYPE_SETTING_PPP))
#define NM_SETTING_PPP_GET_CLASS(obj) \
    (G_TYPE_INSTANCE_GET_CLASS((obj), NM_TYPE_SETTING_PPP, NMSettingPppClass))

#define NM_SETTING_PPP_SETTING_NAME "ppp"

#define NM_SETTING_PPP_NOAUTH            "noauth"
#define NM_SETTING_PPP_REFUSE_EAP        "refuse-eap"
#define NM_SETTING_PPP_REFUSE_PAP        "refuse-pap"
#define NM_SETTING_PPP_REFUSE_CHAP       "refuse-chap"
#define NM_SETTING_PPP_REFUSE_MSCHAP     "refuse-mschap"
#define NM_SETTING_PPP_REFUSE_MSCHAPV2   "refuse-mschapv2"
//APN Gnome Additional code start
#define NM_SETTING_PPP_INITIAL_EPS_BEARER_NOAUTH            "initial-eps-bearer-noauth"
#define NM_SETTING_PPP_INITIAL_EPS_BEARER_REFUSE_EAP        "initial-eps-bearer-refuse-eap"
#define NM_SETTING_PPP_INITIAL_EPS_BEARER_REFUSE_PAP        "initial-eps-bearer-refuse-pap"
#define NM_SETTING_PPP_INITIAL_EPS_BEARER_REFUSE_CHAP       "initial-eps-bearer-refuse-chap"
#define NM_SETTING_PPP_INITIAL_EPS_BEARER_REFUSE_MSCHAP     "initial-eps-bearer-refuse-mschap"
#define NM_SETTING_PPP_INITIAL_EPS_BEARER_REFUSE_MSCHAPV2   "initial-eps-bearer-refuse-mschapv2"
//APN Gnome Additional code end
#define NM_SETTING_PPP_NOBSDCOMP         "nobsdcomp"
#define NM_SETTING_PPP_NODEFLATE         "nodeflate"
#define NM_SETTING_PPP_NO_VJ_COMP        "no-vj-comp"
#define NM_SETTING_PPP_REQUIRE_MPPE      "require-mppe"
#define NM_SETTING_PPP_REQUIRE_MPPE_128  "require-mppe-128"
#define NM_SETTING_PPP_MPPE_STATEFUL     "mppe-stateful"
#define NM_SETTING_PPP_CRTSCTS           "crtscts"
#define NM_SETTING_PPP_BAUD              "baud"
#define NM_SETTING_PPP_MRU               "mru"
#define NM_SETTING_PPP_MTU               "mtu"
#define NM_SETTING_PPP_LCP_ECHO_FAILURE  "lcp-echo-failure"
#define NM_SETTING_PPP_LCP_ECHO_INTERVAL "lcp-echo-interval"

typedef struct _NMSettingPppClass NMSettingPppClass;

GType nm_setting_ppp_get_type(void);

NMSetting *nm_setting_ppp_new(void);
gboolean   nm_setting_ppp_get_noauth(NMSettingPpp *setting);
gboolean   nm_setting_ppp_get_refuse_eap(NMSettingPpp *setting);
gboolean   nm_setting_ppp_get_refuse_pap(NMSettingPpp *setting);
gboolean   nm_setting_ppp_get_refuse_chap(NMSettingPpp *setting);
gboolean   nm_setting_ppp_get_refuse_mschap(NMSettingPpp *setting);
gboolean   nm_setting_ppp_get_refuse_mschapv2(NMSettingPpp *setting);
//APN Gnome Additional code start
NM_AVAILABLE_IN_1_48
gboolean   nm_setting_ppp_get_initial_eps_noauth(NMSettingPpp *setting);
NM_AVAILABLE_IN_1_48
gboolean   nm_setting_ppp_get_initial_eps_refuse_eap(NMSettingPpp *setting);
NM_AVAILABLE_IN_1_48
gboolean   nm_setting_ppp_get_initial_eps_refuse_pap(NMSettingPpp *setting);
NM_AVAILABLE_IN_1_48
gboolean   nm_setting_ppp_get_initial_eps_refuse_chap(NMSettingPpp *setting);
NM_AVAILABLE_IN_1_48
gboolean   nm_setting_ppp_get_initial_eps_refuse_mschap(NMSettingPpp *setting);
NM_AVAILABLE_IN_1_48
gboolean   nm_setting_ppp_get_initial_eps_refuse_mschapv2(NMSettingPpp *setting);
//APN Gnome Additional code end
gboolean   nm_setting_ppp_get_nobsdcomp(NMSettingPpp *setting);
gboolean   nm_setting_ppp_get_nodeflate(NMSettingPpp *setting);
gboolean   nm_setting_ppp_get_no_vj_comp(NMSettingPpp *setting);
gboolean   nm_setting_ppp_get_require_mppe(NMSettingPpp *setting);
gboolean   nm_setting_ppp_get_require_mppe_128(NMSettingPpp *setting);
gboolean   nm_setting_ppp_get_mppe_stateful(NMSettingPpp *setting);
gboolean   nm_setting_ppp_get_crtscts(NMSettingPpp *setting);
guint32    nm_setting_ppp_get_baud(NMSettingPpp *setting);
guint32    nm_setting_ppp_get_mru(NMSettingPpp *setting);
guint32    nm_setting_ppp_get_mtu(NMSettingPpp *setting);
guint32    nm_setting_ppp_get_lcp_echo_failure(NMSettingPpp *setting);
guint32    nm_setting_ppp_get_lcp_echo_interval(NMSettingPpp *setting);

G_END_DECLS

#endif /* __NM_SETTING_PPP_H__ */

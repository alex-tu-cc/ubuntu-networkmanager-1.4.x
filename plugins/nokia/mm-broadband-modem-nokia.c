/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Copyright (C) 2008 - 2009 Novell, Inc.
 * Copyright (C) 2009 - 2011 Red Hat, Inc.
 * Copyright (C) 2011 Google Inc.
 */

#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include "ModemManager.h"
#include "mm-serial-parsers.h"
#include "mm-log.h"
#include "mm-errors-types.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-messaging.h"
#include "mm-iface-modem-3gpp.h"
#include "mm-modem-helpers.h"
#include "mm-base-modem-at.h"
#include "mm-broadband-modem-nokia.h"
#include "mm-sim-nokia.h"

static void iface_modem_init (MMIfaceModem *iface);
static void iface_modem_messaging_init (MMIfaceModemMessaging *iface);

static MMIfaceModem *iface_modem_parent;

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemNokia, mm_broadband_modem_nokia, MM_TYPE_BROADBAND_MODEM, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM, iface_modem_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_MESSAGING, iface_modem_messaging_init));

/*****************************************************************************/
/* Create SIM (Modem interface) */

static MMBaseSim *
create_sim_finish (MMIfaceModem *self,
                   GAsyncResult *res,
                   GError **error)
{
    return mm_sim_nokia_new_finish (res, error);
}

static void
create_sim (MMIfaceModem *self,
            GAsyncReadyCallback callback,
            gpointer user_data)
{
    /* New Nokia SIM */
    mm_sim_nokia_new (MM_BASE_MODEM (self),
                      NULL, /* cancellable */
                      callback,
                      user_data);
}

/*****************************************************************************/
/* Load access technologies (Modem interface) */

typedef struct {
    MMModemAccessTechnology act;
    guint mask;
} AccessTechInfo;

static void
access_tech_set_result (GSimpleAsyncResult *simple,
                        MMModemAccessTechnology act,
                        guint mask)
{
    AccessTechInfo *info;

    info = g_new (AccessTechInfo, 1);
    info->act = act;
    info->mask = mask;

    g_simple_async_result_set_op_res_gpointer (simple, info, g_free);
}

static gboolean
load_access_technologies_finish (MMIfaceModem *self,
                                 GAsyncResult *res,
                                 MMModemAccessTechnology *access_technologies,
                                 guint *mask,
                                 GError **error)
{
    AccessTechInfo *info;

    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return FALSE;

    info = g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res));
    g_assert (info);
    *access_technologies = info->act;
    *mask = info->mask;
    return TRUE;
}

static void
parent_load_access_technologies_ready (MMIfaceModem *self,
                                       GAsyncResult *res,
                                       GSimpleAsyncResult *simple)
{
   MMModemAccessTechnology act = MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
   guint mask = 0;
   GError *error = NULL;

    if (!iface_modem_parent->load_access_technologies_finish (self, res, &act, &mask, &error))
        g_simple_async_result_take_error (simple, error);
    else
        access_tech_set_result (simple, act, MM_IFACE_MODEM_3GPP_ALL_ACCESS_TECHNOLOGIES_MASK);

    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
access_tech_ready (MMBaseModem *self,
                   GAsyncResult *res,
                   GSimpleAsyncResult *simple)
{
    MMModemAccessTechnology act = MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
    const gchar *response, *p;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, NULL);
    if (!response) {
        /* Chain up to parent */
        iface_modem_parent->load_access_technologies (
                                    MM_IFACE_MODEM (self),
                                    (GAsyncReadyCallback)parent_load_access_technologies_ready,
                                    simple);
        return;
    }

    p = mm_strip_tag (response, "*CNTI:");
    p = strchr (p, ',');
    if (p)
        act = mm_string_to_access_tech (p + 1);

    if (act == MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN)
        g_simple_async_result_set_error (
            simple,
            MM_CORE_ERROR,
            MM_CORE_ERROR_FAILED,
            "Couldn't parse access technologies result: '%s'",
            response);
    else
        access_tech_set_result (simple, act, MM_IFACE_MODEM_3GPP_ALL_ACCESS_TECHNOLOGIES_MASK);

    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
load_access_technologies (MMIfaceModem *self,
                          GAsyncReadyCallback callback,
                          gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        load_access_technologies);

    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "*CNTI=0",
                              3,
                              FALSE,
                              (GAsyncReadyCallback)access_tech_ready,
                              result);
}

/*****************************************************************************/
/* Initializing the modem (during first enabling) */

typedef struct {
    GSimpleAsyncResult *result;
    MMBroadbandModemNokia *self;
    guint retries;
} EnablingModemInitContext;

static void
enabling_modem_init_context_complete_and_free (EnablingModemInitContext *ctx)
{
    g_simple_async_result_complete (ctx->result);
    g_object_unref (ctx->result);
    g_object_unref (ctx->self);
    g_slice_free (EnablingModemInitContext, ctx);
}

static gboolean
enabling_modem_init_finish (MMBroadbandModem *self,
                            GAsyncResult *res,
                            GError **error)

{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void retry_atz (EnablingModemInitContext *ctx);

static void
atz_ready (MMBaseModem *self,
           GAsyncResult *res,
           EnablingModemInitContext *ctx)
{
    GError *error = NULL;

    /* One retry less */
    ctx->retries--;

    if (!mm_base_modem_at_command_full_finish (self, res, &error)) {
        /* Consumed all retries... */
        if (ctx->retries == 0) {
            g_simple_async_result_take_error (ctx->result, error);
            enabling_modem_init_context_complete_and_free (ctx);
            return;
        }

        /* Retry... */
        g_error_free (error);
        retry_atz (ctx);
        return;
    }

    /* Good! */
    g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);
    enabling_modem_init_context_complete_and_free (ctx);
}

static void
retry_atz (EnablingModemInitContext *ctx)
{
    mm_base_modem_at_command_full (MM_BASE_MODEM (ctx->self),
                                   mm_base_modem_peek_port_primary (MM_BASE_MODEM (ctx->self)),
                                   "Z",
                                   6,
                                   FALSE,
                                   FALSE,
                                   NULL, /* cancellable */
                                   (GAsyncReadyCallback)atz_ready,
                                   ctx);
}

static void
enabling_modem_init (MMBroadbandModem *self,
                     GAsyncReadyCallback callback,
                     gpointer user_data)
{
    EnablingModemInitContext *ctx;

    ctx = g_slice_new0 (EnablingModemInitContext);
    ctx->result = g_simple_async_result_new (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             enabling_modem_init);
    ctx->self = g_object_ref (self);

    /* Send the init command twice; some devices (Nokia N900) appear to take a
     * few commands before responding correctly.  Instead of penalizing them for
     * being stupid the first time by failing to enable the device, just
     * try again. */
    ctx->retries = 2;
    retry_atz (ctx);
}

/*****************************************************************************/
/* Setup ports (Broadband modem class) */

static const gchar *primary_init_sequence[] = {
    /* When initializing a Nokia port, first enable the echo,
     * and then disable it, so that we get it properly disabled. */
    "E1 E0",
    /* The N900 ignores the E0 when it's on the same line as the E1, so try again */
    "E0",
    /* Get word responses */
    "V1",
    /* Extended numeric codes */
    "+CMEE=1",
    /* Report all call status */
    "X4",
    /* Assert DCD when carrier detected */
    "&C1",
    NULL
};

static void
setup_ports (MMBroadbandModem *self)
{
    MMPortSerialAt *primary;

    /* Call parent's setup ports first always */
    MM_BROADBAND_MODEM_CLASS (mm_broadband_modem_nokia_parent_class)->setup_ports (self);

    primary = mm_base_modem_peek_port_primary (MM_BASE_MODEM (self));

    g_object_set (primary,
                  MM_PORT_SERIAL_AT_INIT_SEQUENCE, primary_init_sequence,
                  NULL);
}

/*****************************************************************************/

MMBroadbandModemNokia *
mm_broadband_modem_nokia_new (const gchar *device,
                              const gchar **drivers,
                              const gchar *plugin,
                              guint16 vendor_id,
                              guint16 product_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_NOKIA,
                         MM_BASE_MODEM_DEVICE, device,
                         MM_BASE_MODEM_DRIVERS, drivers,
                         MM_BASE_MODEM_PLUGIN, plugin,
                         MM_BASE_MODEM_VENDOR_ID, vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         NULL);
}

static void
mm_broadband_modem_nokia_init (MMBroadbandModemNokia *self)
{
}

static void
iface_modem_messaging_init (MMIfaceModemMessaging *iface)
{
    /* Don't even try to check messaging support */
    iface->check_support = NULL;
    iface->check_support_finish = NULL;
}

static void
iface_modem_init (MMIfaceModem *iface)
{
    iface_modem_parent = g_type_interface_peek_parent (iface);

    /* Create Nokia-specific SIM*/
    iface->create_sim = create_sim;
    iface->create_sim_finish = create_sim_finish;

    /* Nokia handsets (at least N85) do not support "power on"; they do
     * support "power off" but you proabably do not want to turn off the
     * power on your telephone if something went wrong with connecting
     * process. So, disabling both these operations.  The Nokia GSM/UMTS command
     * reference v1.2 also states that only CFUN=0 (turn off but still charge)
     * and CFUN=1 (full functionality) are supported, and since the phone has
     * to be in CFUN=1 before we'll be able to talk to it in the first place,
     * we shouldn't bother with CFUN at all.
     */
    iface->load_power_state = NULL;
    iface->load_power_state_finish = NULL;
    iface->modem_power_up = NULL;
    iface->modem_power_up_finish = NULL;
    iface->modem_power_down = NULL;
    iface->modem_power_down_finish = NULL;

    iface->load_access_technologies = load_access_technologies;
    iface->load_access_technologies_finish = load_access_technologies_finish;
}

static void
mm_broadband_modem_nokia_class_init (MMBroadbandModemNokiaClass *klass)
{
    MMBroadbandModemClass *broadband_modem_class = MM_BROADBAND_MODEM_CLASS (klass);

    broadband_modem_class->setup_ports = setup_ports;
    broadband_modem_class->enabling_modem_init = enabling_modem_init;
    broadband_modem_class->enabling_modem_init_finish = enabling_modem_init_finish;
}

/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Cosimo Cecchi <cosimoc@gnome.org>
 */

#ifndef _CC_SEARCH_LOCATIONS_DIALOG_H
#define _CC_SEARCH_LOCATIONS_DIALOG_H

#include "cc-search-panel.h"

#define CC_SEARCH_LOCATIONS_DIALOG_TYPE (cc_search_locations_dialog_get_type ())
#define CC_SEARCH_LOCATIONS_DIALOG(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), CC_SEARCH_LOCATIONS_DIALOG_TYPE, CcSearchLocationsDialog))

typedef struct _CcSearchLocationsDialog      CcSearchLocationsDialog;
typedef struct _CcSearchLocationsDialogClass CcSearchLocationsDialogClass;

GType                    cc_search_locations_dialog_get_type   (void);

CcSearchLocationsDialog *cc_search_locations_dialog_new        (CcSearchPanel *panel);

gboolean cc_search_locations_dialog_is_available (void);

#endif /* _CC_SEARCH_LOCATIONS_DIALOG_H */

/* this file is part of evince, a gnome document viewer
 *
 *  Copyright (C) 2005 Red Hat, Inc
 *
 * Evince is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Evince is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
 */

#ifndef __EV_JOB_QUEUE_H__
#define __EV_JOB_QUEUE_H__

#include <gtk/gtk.h>
#include "ev-jobs.h"

G_BEGIN_DECLS


void     ev_job_queue_init       (void);

void     ev_job_queue_add_job    (EvJob         *job,
				  EvJobPriority  priority);
gboolean ev_job_queue_update_job (EvJob         *job,
				  EvJobPriority  new_priority);
gboolean ev_job_queue_remove_job (EvJob         *job);

G_END_DECLS

#endif /* __EV_JOB_QUEUE_H__ */
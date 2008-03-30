/* acme-volume-dummy.c

   Copyright (C) 2002, 2003 Bastien Nocera

   The Gnome Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   The Gnome Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with the Gnome Library; see the file COPYING.LIB.  If not,
   write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.

   Author: Bastien Nocera <hadess@hadess.net>
 */

#include "config.h"
#include "acme-volume-dummy.h"

static GObjectClass *parent_class = NULL;

G_DEFINE_TYPE (AcmeVolumeDummy, acme_volume_dummy, ACME_TYPE_VOLUME)

static void
acme_volume_dummy_finalize (GObject *object)
{
	g_return_if_fail (object != NULL);
	g_return_if_fail (ACME_IS_VOLUME_DUMMY (object));

	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
acme_volume_dummy_set_mute (AcmeVolume *vol, gboolean val)
{
}

static gboolean
acme_volume_dummy_get_mute (AcmeVolume *vol)
{
	return FALSE;
}

static int
acme_volume_dummy_get_volume (AcmeVolume *vol)
{
	return 0;
}

static void
acme_volume_dummy_set_volume (AcmeVolume *vol, int val)
{
}

static void
acme_volume_dummy_init (AcmeVolumeDummy *vol)
{
}

static void
acme_volume_dummy_class_init (AcmeVolumeDummyClass *klass)
{
	AcmeVolumeClass *volume_class = ACME_VOLUME_CLASS (klass);
	G_OBJECT_CLASS (klass)->finalize = acme_volume_dummy_finalize;

	parent_class = g_type_class_peek_parent (klass);

	volume_class->set_volume = acme_volume_dummy_set_volume;
	volume_class->get_volume = acme_volume_dummy_get_volume;
	volume_class->set_mute = acme_volume_dummy_set_mute;
	volume_class->get_mute = acme_volume_dummy_get_mute;
}

/*
 * Copyright (C) 2010 Intel, Inc
 * Copyright (C) 2016 Endless, Inc
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
 * Author: Thomas Wood <thomas.wood@intel.com>
 *         Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *
 */

#include <glib/gi18n.h>

#include "cc-keyboard-manager.h"
#include "keyboard-shortcuts.h"
#include "wm-common.h"

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

#define BINDINGS_SCHEMA       "org.gnome.settings-daemon.plugins.media-keys"
#define CUSTOM_SHORTCUTS_ID   "custom"

struct _CcKeyboardManager
{
  GObject             parent;

  GtkListStore       *shortcuts_model;
  GtkListStore       *sections_store;

  GHashTable         *kb_system_sections;
  GHashTable         *kb_apps_sections;
  GHashTable         *kb_user_sections;

  GSettings          *binding_settings;

  gpointer            wm_changed_id;
};

G_DEFINE_TYPE (CcKeyboardManager, cc_keyboard_manager, G_TYPE_OBJECT)

enum
{
  SHORTCUT_ADDED,
  SHORTCUT_CHANGED,
  SHORTCUT_REMOVED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

/*
 * Auxiliary methos
 */
static void
free_key_array (GPtrArray *keys)
{
  if (keys != NULL)
    {
      gint i;

      for (i = 0; i < keys->len; i++)
        {
          CcKeyboardItem *item;

          item = g_ptr_array_index (keys, i);

          g_object_unref (item);
        }

      g_ptr_array_free (keys, TRUE);
    }
}

static const gchar*
get_binding_from_variant (GVariant *variant)
{
  if (g_variant_is_of_type (variant, G_VARIANT_TYPE_STRING))
    return g_variant_get_string (variant, NULL);
  else if (g_variant_is_of_type (variant, G_VARIANT_TYPE_STRING_ARRAY))
    return g_variant_get_strv (variant, NULL)[0];

  return NULL;
}

static gboolean
is_shortcut_different (CcUniquenessData *data,
                       CcKeyboardItem   *item)
{
  if (data->orig_item && cc_keyboard_item_equal (data->orig_item, item))
    return FALSE;

  if (data->new_keyval != 0)
    {
      if (data->new_keyval != item->keyval)
        return TRUE;
    }
  else if (item->keyval != 0 || data->new_keycode != item->keycode)
    {
      return TRUE;
    }

  return FALSE;
}

static gboolean
compare_keys_for_uniqueness (CcKeyboardItem   *current_item,
                             CcUniquenessData *data)
{
  CcKeyboardItem *reverse_item;

  /* No conflict for: blanks, different modifiers or ourselves */
  if (!current_item ||
      data->orig_item == current_item ||
      data->new_mask != current_item->mask)
    {
      return FALSE;
    }

  reverse_item = cc_keyboard_item_get_reverse_item (current_item);

  /* When the current item is the reversed shortcut of a main item, simply ignore it */
  if (reverse_item && cc_keyboard_item_is_hidden (current_item))
    return FALSE;

  if (is_shortcut_different (data, current_item))
    return FALSE;

  /* Also check for the reverse item if any */
  if (reverse_item && is_shortcut_different (data, reverse_item))
    return FALSE;

  /* No tests failed and we found a conflict */
  data->conflict_item = current_item;

  return TRUE;
}

static gboolean
check_for_uniqueness (gpointer          key,
                      GPtrArray        *keys_array,
                      CcUniquenessData *data)
{
  guint i;

  for (i = 0; i < keys_array->len; i++)
    {
      CcKeyboardItem *item;

      item = keys_array->pdata[i];

      if (compare_keys_for_uniqueness (item, data))
        return TRUE;
    }

  return FALSE;
}


static GHashTable*
get_hash_for_group (CcKeyboardManager *self,
                    BindingGroupType   group)
{
  GHashTable *hash;

  switch (group)
    {
    case BINDING_GROUP_SYSTEM:
      hash = self->kb_system_sections;
      break;
    case BINDING_GROUP_APPS:
      hash = self->kb_apps_sections;
      break;
    case BINDING_GROUP_USER:
      hash = self->kb_user_sections;
      break;
    default:
      hash = NULL;
    }

  return hash;
}

static gboolean
have_key_for_group (CcKeyboardManager *self,
                    int                group,
                    const gchar       *name)
{
  GHashTableIter iter;
  GPtrArray *keys;
  gint i;

  g_hash_table_iter_init (&iter, get_hash_for_group (self, group));
  while (g_hash_table_iter_next (&iter, NULL, (gpointer*) &keys))
    {
      for (i = 0; i < keys->len; i++)
        {
          CcKeyboardItem *item = g_ptr_array_index (keys, i);

          if (item->type == CC_KEYBOARD_ITEM_TYPE_GSETTINGS &&
              g_strcmp0 (name, item->key) == 0)
            {
              return TRUE;
            }
        }
    }

  return FALSE;
}

static void
add_shortcuts (CcKeyboardManager *self)
{
  GtkTreeModel *sections_model;
  GtkTreeIter sections_iter;
  gboolean can_continue;

  sections_model = GTK_TREE_MODEL (self->sections_store);
  can_continue = gtk_tree_model_get_iter_first (sections_model, &sections_iter);

  while (can_continue)
    {
      BindingGroupType group;
      GPtrArray *keys;
      gchar *id, *title;
      gint i;

      gtk_tree_model_get (sections_model,
                          &sections_iter,
                          SECTION_DESCRIPTION_COLUMN, &title,
                          SECTION_GROUP_COLUMN, &group,
                          SECTION_ID_COLUMN, &id,
                          -1);

      /* Ignore separators */
      if (group == BINDING_GROUP_SEPARATOR)
        {
          can_continue = gtk_tree_model_iter_next (sections_model, &sections_iter);
          continue;
        }

      keys = g_hash_table_lookup (get_hash_for_group (self, group), id);

      for (i = 0; i < keys->len; i++)
        {
          CcKeyboardItem *item = g_ptr_array_index (keys, i);

          if (!cc_keyboard_item_is_hidden (item))
            {
              GtkTreeIter new_row;

              gtk_list_store_append (self->shortcuts_model, &new_row);
              gtk_list_store_set (self->shortcuts_model,
                                  &new_row,
                                  DETAIL_DESCRIPTION_COLUMN, item->description,
                                  DETAIL_KEYENTRY_COLUMN, item,
                                  DETAIL_TYPE_COLUMN, SHORTCUT_TYPE_KEY_ENTRY,
                                  -1);

              g_signal_emit (self, signals[SHORTCUT_ADDED],
                             0,
                             item,
                             id,
                             title);
            }
        }

      can_continue = gtk_tree_model_iter_next (sections_model, &sections_iter);

      g_free (title);
      g_free (id);
    }
}

static void
append_section (CcKeyboardManager  *self,
                const gchar        *title,
                const gchar        *id,
                BindingGroupType    group,
                const KeyListEntry *keys_list)
{
  GtkTreeModel *shortcut_model;
  GtkTreeIter iter;
  GHashTable *reverse_items;
  GHashTable *hash;
  GPtrArray *keys_array;
  gboolean is_new;
  gint i;

  hash = get_hash_for_group (self, group);

  if (!hash)
    return;

  shortcut_model = GTK_TREE_MODEL (self->shortcuts_model);

  /* Add all CcKeyboardItems for this section */
  is_new = FALSE;
  keys_array = g_hash_table_lookup (hash, id);
  if (keys_array == NULL)
    {
      keys_array = g_ptr_array_new ();
      is_new = TRUE;
    }

  reverse_items = g_hash_table_new (g_str_hash, g_str_equal);

  for (i = 0; keys_list != NULL && keys_list[i].name != NULL; i++)
    {
      CcKeyboardItem *item;
      gboolean ret;

      if (have_key_for_group (self, group, keys_list[i].name))
        continue;

      item = cc_keyboard_item_new (keys_list[i].type);

      switch (keys_list[i].type)
        {
        case CC_KEYBOARD_ITEM_TYPE_GSETTINGS_PATH:
          ret = cc_keyboard_item_load_from_gsettings_path (item, keys_list[i].name, FALSE);
          break;

        case CC_KEYBOARD_ITEM_TYPE_GSETTINGS:
          ret = cc_keyboard_item_load_from_gsettings (item,
                                                      keys_list[i].description,
                                                      keys_list[i].schema,
                                                      keys_list[i].name);
          if (ret && keys_list[i].reverse_entry != NULL)
            {
              CcKeyboardItem *reverse_item;
              reverse_item = g_hash_table_lookup (reverse_items,
                                                  keys_list[i].reverse_entry);
              if (reverse_item != NULL)
                {
                  cc_keyboard_item_add_reverse_item (item,
                                                     reverse_item,
                                                     keys_list[i].is_reversed);
                }
              else
                {
                  g_hash_table_insert (reverse_items,
                                       keys_list[i].name,
                                       item);
                }
            }
          break;

        default:
          g_assert_not_reached ();
        }

      if (ret == FALSE)
        {
          /* We don't actually want to popup a dialog - just skip this one */
          g_object_unref (item);
          continue;
        }

      cc_keyboard_item_set_hidden (item, keys_list[i].hidden);
      item->model = shortcut_model;
      item->group = group;

      g_ptr_array_add (keys_array, item);
    }

  g_hash_table_destroy (reverse_items);

  /* Add the keys to the hash table */
  if (is_new)
    {
      g_hash_table_insert (hash, g_strdup (id), keys_array);

      /* Append the section to the left tree view */
      gtk_list_store_append (GTK_LIST_STORE (self->sections_store), &iter);
      gtk_list_store_set (GTK_LIST_STORE (self->sections_store),
                          &iter,
                          SECTION_DESCRIPTION_COLUMN, title,
                          SECTION_ID_COLUMN, id,
                          SECTION_GROUP_COLUMN, group,
                          -1);
    }
}

static void
append_sections_from_file (CcKeyboardManager  *self,
                           const gchar        *path,
                           const char         *datadir,
                           gchar             **wm_keybindings)
{
  KeyList *keylist;
  KeyListEntry *keys;
  KeyListEntry key = { 0, 0, 0, 0, 0, 0, 0 };
  const char *title;
  int group;
  guint i;

  keylist = parse_keylist_from_file (path);

  if (keylist == NULL)
    return;

#define const_strv(s) ((const gchar* const*) s)

  /* If there's no keys to add, or the settings apply to a window manager
   * that's not the one we're running */
  if (keylist->entries->len == 0 ||
      (keylist->wm_name != NULL && !g_strv_contains (const_strv (wm_keybindings), keylist->wm_name)) ||
      keylist->name == NULL)
    {
      g_free (keylist->name);
      g_free (keylist->package);
      g_free (keylist->wm_name);
      g_array_free (keylist->entries, TRUE);
      g_free (keylist);
      return;
    }

#undef const_strv

  /* Empty KeyListEntry to end the array */
  key.name = NULL;
  g_array_append_val (keylist->entries, key);

  keys = (KeyListEntry *) g_array_free (keylist->entries, FALSE);
  if (keylist->package)
    {
      char *localedir;

      localedir = g_build_filename (datadir, "locale", NULL);
      bindtextdomain (keylist->package, localedir);
      g_free (localedir);

      title = dgettext (keylist->package, keylist->name);
    } else {
      title = _(keylist->name);
    }

  if (keylist->group && strcmp (keylist->group, "system") == 0)
    group = BINDING_GROUP_SYSTEM;
  else
    group = BINDING_GROUP_APPS;

  append_section (self, title, keylist->name, group, keys);

  g_free (keylist->name);
  g_free (keylist->package);
  g_free (keylist->wm_name);
  g_free (keylist->schema);
  g_free (keylist->group);

  for (i = 0; keys[i].name != NULL; i++)
    {
      KeyListEntry *entry = &keys[i];
      g_free (entry->schema);
      g_free (entry->description);
      g_free (entry->name);
      g_free (entry->reverse_entry);
    }

  g_free (keylist);
  g_free (keys);
}

static void
append_sections_from_gsettings (CcKeyboardManager *self)
{
  char **custom_paths;
  GArray *entries;
  KeyListEntry key = { 0, 0, 0, 0, 0, 0, 0 };
  int i;

  /* load custom shortcuts from GSettings */
  entries = g_array_new (FALSE, TRUE, sizeof (KeyListEntry));

  custom_paths = g_settings_get_strv (self->binding_settings, "custom-keybindings");
  for (i = 0; custom_paths[i]; i++)
    {
      key.name = g_strdup (custom_paths[i]);
      if (!have_key_for_group (self, BINDING_GROUP_USER, key.name))
        {
          key.type = CC_KEYBOARD_ITEM_TYPE_GSETTINGS_PATH;
          g_array_append_val (entries, key);
        }
      else
        g_free (key.name);
    }
  g_strfreev (custom_paths);

  if (entries->len > 0)
    {
      KeyListEntry *keys;
      int i;

      /* Empty KeyListEntry to end the array */
      key.name = NULL;
      g_array_append_val (entries, key);

      keys = (KeyListEntry *) entries->data;
      append_section (self, _("Custom Shortcuts"), CUSTOM_SHORTCUTS_ID, BINDING_GROUP_USER, keys);
      for (i = 0; i < entries->len; ++i)
        {
          g_free (keys[i].name);
        }
    }
  else
    {
      append_section (self, _("Custom Shortcuts"), CUSTOM_SHORTCUTS_ID, BINDING_GROUP_USER, NULL);
    }

  g_array_free (entries, TRUE);
}

static void
reload_sections (CcKeyboardManager *self)
{
  GtkTreeModel *shortcut_model;
  GHashTable *loaded_files;
  GDir *dir;
  gchar *default_wm_keybindings[] = { "Mutter", "GNOME Shell", NULL };
  gchar **wm_keybindings;
  const gchar * const * data_dirs;
  guint i;

  shortcut_model = GTK_TREE_MODEL (self->shortcuts_model);

  /* Clear previous models and hash tables */
  gtk_list_store_clear (GTK_LIST_STORE (self->sections_store));
  gtk_list_store_clear (GTK_LIST_STORE (shortcut_model));

  g_clear_pointer (&self->kb_system_sections, g_hash_table_destroy);
  self->kb_system_sections = g_hash_table_new_full (g_str_hash,
                                                    g_str_equal,
                                                    g_free,
                                                    (GDestroyNotify) free_key_array);

  g_clear_pointer (&self->kb_apps_sections, g_hash_table_destroy);
  self->kb_apps_sections = g_hash_table_new_full (g_str_hash,
                                                  g_str_equal,
                                                  g_free,
                                                  (GDestroyNotify) free_key_array);

  g_clear_pointer (&self->kb_user_sections, g_hash_table_destroy);
  self->kb_user_sections = g_hash_table_new_full (g_str_hash,
                                                  g_str_equal,
                                                  g_free,
                                                  (GDestroyNotify) free_key_array);

  /* Load WM keybindings */
#ifdef GDK_WINDOWING_X11
  if (GDK_IS_X11_DISPLAY (gdk_display_get_default ()))
    wm_keybindings = wm_common_get_current_keybindings ();
  else
#endif
    wm_keybindings = g_strdupv (default_wm_keybindings);

  loaded_files = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  data_dirs = g_get_system_data_dirs ();
  for (i = 0; data_dirs[i] != NULL; i++)
    {
      char *dir_path;
      const gchar *name;

      dir_path = g_build_filename (data_dirs[i], "gnome-control-center", "keybindings", NULL);

      dir = g_dir_open (dir_path, 0, NULL);
      if (!dir)
        {
          g_free (dir_path);
          continue;
        }

      for (name = g_dir_read_name (dir) ; name ; name = g_dir_read_name (dir))
        {
          gchar *path;

          if (g_str_has_suffix (name, ".xml") == FALSE)
            continue;

          if (g_hash_table_lookup (loaded_files, name) != NULL)
            {
              g_debug ("Not loading %s, it was already loaded from another directory", name);
              continue;
            }

          g_hash_table_insert (loaded_files, g_strdup (name), GINT_TO_POINTER (1));
          path = g_build_filename (dir_path, name, NULL);
          append_sections_from_file (self, path, data_dirs[i], wm_keybindings);
          g_free (path);
        }

      g_free (dir_path);
      g_dir_close (dir);
    }

  g_hash_table_destroy (loaded_files);
  g_strfreev (wm_keybindings);

  /* Load custom keybindings */
  append_sections_from_gsettings (self);
}

/*
 * Callbacks
 */
static void
on_window_manager_change (const char        *wm_name,
                          CcKeyboardManager *self)
{
  reload_sections (self);
}

static void
cc_keyboard_manager_finalize (GObject *object)
{
  CcKeyboardManager *self = (CcKeyboardManager *)object;

  g_clear_pointer (&self->kb_system_sections, g_hash_table_destroy);
  g_clear_pointer (&self->kb_apps_sections, g_hash_table_destroy);
  g_clear_pointer (&self->kb_user_sections, g_hash_table_destroy);
  g_clear_object (&self->binding_settings);

  g_clear_pointer (&self->wm_changed_id, wm_common_unregister_window_manager_change);

  G_OBJECT_CLASS (cc_keyboard_manager_parent_class)->finalize (object);
}

static void
cc_keyboard_manager_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

static void
cc_keyboard_manager_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

static void
cc_keyboard_manager_class_init (CcKeyboardManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = cc_keyboard_manager_finalize;
  object_class->get_property = cc_keyboard_manager_get_property;
  object_class->set_property = cc_keyboard_manager_set_property;

  /**
   * CcKeyboardManager:shortcut-added:
   *
   * Emited when a shortcut is added.
   */
  signals[SHORTCUT_ADDED] = g_signal_new ("shortcut-added",
                                          CC_TYPE_KEYBOARD_MANAGER,
                                          G_SIGNAL_RUN_FIRST,
                                          0, NULL, NULL, NULL,
                                          G_TYPE_NONE,
                                          3,
                                          CC_TYPE_KEYBOARD_ITEM,
                                          G_TYPE_STRING,
                                          G_TYPE_STRING);

  /**
   * CcKeyboardManager:shortcut-changed:
   *
   * Emited when a shortcut is added.
   */
  signals[SHORTCUT_CHANGED] = g_signal_new ("shortcut-changed",
                                            CC_TYPE_KEYBOARD_MANAGER,
                                            G_SIGNAL_RUN_FIRST,
                                            0, NULL, NULL, NULL,
                                            G_TYPE_NONE,
                                            1,
                                            CC_TYPE_KEYBOARD_ITEM);


  /**
   * CcKeyboardManager:shortcut-removed:
   *
   * Emited when a shortcut is removed.
   */
  signals[SHORTCUT_REMOVED] = g_signal_new ("shortcut-removed",
                                            CC_TYPE_KEYBOARD_MANAGER,
                                            G_SIGNAL_RUN_FIRST,
                                            0, NULL, NULL, NULL,
                                            G_TYPE_NONE,
                                            1,
                                            CC_TYPE_KEYBOARD_ITEM);
}

static void
cc_keyboard_manager_init (CcKeyboardManager *self)
{
  /* Bindings */
  self->binding_settings = g_settings_new (BINDINGS_SCHEMA);

  /* Setup the section models */
  self->sections_store = gtk_list_store_new (SECTION_N_COLUMNS,
                                             G_TYPE_STRING,
                                             G_TYPE_STRING,
                                             G_TYPE_INT);

  self->shortcuts_model = gtk_list_store_new (DETAIL_N_COLUMNS,
                                              G_TYPE_STRING,
                                              G_TYPE_POINTER,
                                              G_TYPE_INT);

#ifdef GDK_WINDOWING_X11
  if (GDK_IS_X11_DISPLAY (gdk_display_get_default ()))
    self->wm_changed_id = wm_common_register_window_manager_change ((GFunc) on_window_manager_change,
                                                                    self);
#endif
}


CcKeyboardManager *
cc_keyboard_manager_new (void)
{
  return g_object_new (CC_TYPE_KEYBOARD_MANAGER, NULL);
}

void
cc_keyboard_manager_load_shortcuts (CcKeyboardManager *self)
{
  g_return_if_fail (CC_IS_KEYBOARD_MANAGER (self));

  reload_sections (self);
  add_shortcuts (self);
}

/**
 * cc_keyboard_manager_create_custom_shortcut:
 * @self: a #CcKeyboardPanel
 *
 * Creates a new temporary keyboard shortcut.
 *
 * Returns: (transfer full): a #CcKeyboardItem
 */
CcKeyboardItem*
cc_keyboard_manager_create_custom_shortcut (CcKeyboardManager *self)
{
  CcKeyboardItem *item;
  gchar *settings_path;

  g_return_val_if_fail (CC_IS_KEYBOARD_MANAGER (self), NULL);

  item = cc_keyboard_item_new (CC_KEYBOARD_ITEM_TYPE_GSETTINGS_PATH);

  settings_path = find_free_settings_path (self->binding_settings);
  cc_keyboard_item_load_from_gsettings_path (item, settings_path, TRUE);
  g_free (settings_path);

  item->model = GTK_TREE_MODEL (self->shortcuts_model);
  item->group = BINDING_GROUP_USER;

  return item;
}

/**
 * cc_keyboard_manager_add_custom_shortcut:
 * @self: a #CcKeyboardPanel
 * @item: the #CcKeyboardItem to be added
 *
 * Effectively adds the custom shortcut.
 */
void
cc_keyboard_manager_add_custom_shortcut (CcKeyboardManager *self,
                                         CcKeyboardItem    *item)
{
  GPtrArray *keys_array;
  GtkTreeIter iter;
  GHashTable *hash;
  GVariantBuilder builder;
  char **settings_paths;
  int i;

  g_return_if_fail (CC_IS_KEYBOARD_MANAGER (self));

  hash = get_hash_for_group (self, BINDING_GROUP_USER);
  keys_array = g_hash_table_lookup (hash, CUSTOM_SHORTCUTS_ID);

  if (keys_array == NULL)
    {
      keys_array = g_ptr_array_new ();
      g_hash_table_insert (hash, g_strdup (CUSTOM_SHORTCUTS_ID), keys_array);
    }

  g_ptr_array_add (keys_array, item);

  gtk_list_store_append (self->shortcuts_model, &iter);
  gtk_list_store_set (self->shortcuts_model, &iter, DETAIL_KEYENTRY_COLUMN, item, -1);

  settings_paths = g_settings_get_strv (self->binding_settings, "custom-keybindings");

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("as"));

  for (i = 0; settings_paths[i]; i++)
    g_variant_builder_add (&builder, "s", settings_paths[i]);

  g_variant_builder_add (&builder, "s", item->gsettings_path);

  g_settings_set_value (self->binding_settings, "custom-keybindings", g_variant_builder_end (&builder));

  g_signal_emit (self, signals[SHORTCUT_ADDED],
                 0,
                 item,
                 CUSTOM_SHORTCUTS_ID,
                 _("Custom Shortcuts"));
}

/**
 * cc_keyboard_manager_remove_custom_shortcut:
 * @self: a #CcKeyboardPanel
 * @item: the #CcKeyboardItem to be added
 *
 * Removed the custom shortcut.
 */
void
cc_keyboard_manager_remove_custom_shortcut  (CcKeyboardManager *self,
                                             CcKeyboardItem    *item)
{
  GtkTreeModel *model;
  GtkTreeIter iter;
  GPtrArray *keys_array;
  GVariantBuilder builder;
  gboolean valid;
  char **settings_paths;
  int i;

  g_return_if_fail (CC_IS_KEYBOARD_MANAGER (self));

  model = GTK_TREE_MODEL (self->shortcuts_model);
  valid = gtk_tree_model_get_iter_first (model, &iter);

  /* Search for the iter */
  while (valid)
    {
      CcKeyboardItem  *current_item;

      gtk_tree_model_get (model, &iter,
                          DETAIL_KEYENTRY_COLUMN, &current_item,
                          -1);

      if (current_item == item)
        break;

      valid = gtk_tree_model_iter_next (model, &iter);
    }

  /* Shortcut not found or not a custom shortcut */
  g_assert (valid);
  g_assert (item->type == CC_KEYBOARD_ITEM_TYPE_GSETTINGS_PATH);

  g_settings_delay (item->settings);
  g_settings_reset (item->settings, "name");
  g_settings_reset (item->settings, "command");
  g_settings_reset (item->settings, "binding");
  g_settings_apply (item->settings);
  g_settings_sync ();

  settings_paths = g_settings_get_strv (self->binding_settings, "custom-keybindings");
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("as"));

  for (i = 0; settings_paths[i]; i++)
    if (strcmp (settings_paths[i], item->gsettings_path) != 0)
      g_variant_builder_add (&builder, "s", settings_paths[i]);

  g_settings_set_value (self->binding_settings,
                        "custom-keybindings",
                        g_variant_builder_end (&builder));

  g_strfreev (settings_paths);

  keys_array = g_hash_table_lookup (get_hash_for_group (self, BINDING_GROUP_USER), CUSTOM_SHORTCUTS_ID);
  g_ptr_array_remove (keys_array, item);

  gtk_list_store_remove (GTK_LIST_STORE (model), &iter);

  g_signal_emit (self, signals[SHORTCUT_REMOVED], 0, item);
}

/**
 * cc_keyboard_manager_get_collision:
 * @self: a #CcKeyboardManager
 * @item: (nullable): a keyboard shortcut
 * @keyval: the key value
 * @mask: a mask for the key sequence
 * @keycode: the code of the key.
 *
 * Retrieves the collision item for the given shortcut.
 *
 * Returns: (transfer none)(nullable): the collisioned shortcut
 */
CcKeyboardItem*
cc_keyboard_manager_get_collision (CcKeyboardManager *self,
                                   CcKeyboardItem    *item,
                                   gint               keyval,
                                   GdkModifierType    mask,
                                   gint               keycode)
{
  CcUniquenessData data;
  BindingGroupType i;

  g_return_val_if_fail (CC_IS_KEYBOARD_MANAGER (self), NULL);

  data.orig_item = item;
  data.new_keyval = keyval;
  data.new_mask = mask;
  data.new_keycode = keycode;
  data.conflict_item = NULL;

  if (keyval == 0 && keycode == 0)
    return NULL;

  /* Any number of shortcuts can be disabled */
  for (i = BINDING_GROUP_SYSTEM; i <= BINDING_GROUP_USER && !data.conflict_item; i++)
    {
      GHashTable *table;

      table = get_hash_for_group (self, i);

      if (!table)
        continue;

      g_hash_table_find (table, (GHRFunc) check_for_uniqueness, &data);
    }

  return data.conflict_item;
}

/**
 * cc_keyboard_manager_disable_shortcut:
 * @self: a #CcKeyboardManager
 * @item: a @CcKeyboardItem
 *
 * Disables the given keyboard shortcut.
 */
void
cc_keyboard_manager_disable_shortcut (CcKeyboardManager *self,
                                      CcKeyboardItem    *item)
{
  g_return_if_fail (CC_IS_KEYBOARD_MANAGER (self));

  g_object_set (item, "binding", NULL, NULL);
}

/**
 * cc_keyboard_manager_reset_shortcut:
 * @self: a #CcKeyboardManager
 * @item: a #CcKeyboardItem
 *
 * Resets the keyboard shortcut managed by @item, and eventually
 * disables any shortcut that conflicts with the new shortcut's
 * value.
 */
void
cc_keyboard_manager_reset_shortcut (CcKeyboardManager *self,
                                    CcKeyboardItem    *item)
{
  GVariant *default_value;
  const gchar *default_binding;

  g_return_if_fail (CC_IS_KEYBOARD_MANAGER (self));
  g_return_if_fail (CC_IS_KEYBOARD_ITEM (item));

  default_value = g_settings_get_default_value (item->settings, item->key);
  default_binding = get_binding_from_variant (default_value);

  /* Disables any shortcut that conflicts with the new shortcut's value */
  if (default_binding && *default_binding != '\0')
    {
      GdkModifierType mask;
      CcKeyboardItem *collision;
      guint *keycodes;
      guint keyval;

      gtk_accelerator_parse_with_keycode (default_binding, &keyval, &keycodes, &mask);

      collision = cc_keyboard_manager_get_collision (self,
                                                     NULL,
                                                     keyval,
                                                     mask,
                                                     keycodes ? keycodes[0] : 0);

      if (collision)
        cc_keyboard_manager_disable_shortcut (self, collision);

      g_free (keycodes);
    }

  /* Resets the current item */
  cc_keyboard_item_reset (item);

  g_variant_unref (default_value);
}

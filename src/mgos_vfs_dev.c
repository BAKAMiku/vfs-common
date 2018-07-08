/*
 * Copyright (c) 2014-2018 Cesanta Software Limited
 * All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the ""License"");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an ""AS IS"" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string.h>

#include "mgos_vfs_dev.h"

#include "common/cs_dbg.h"
#include "common/mg_str.h"
#include "common/queue.h"

struct mgos_vfs_dev_type_entry {
  const char *type;
  const struct mgos_vfs_dev_ops *ops;
  SLIST_ENTRY(mgos_vfs_dev_type_entry) next;
};

static SLIST_HEAD(s_dev_types, mgos_vfs_dev_type_entry)
    s_dev_types = SLIST_HEAD_INITIALIZER(s_dev_types);
static SLIST_HEAD(s_devs, mgos_vfs_dev) s_devs = SLIST_HEAD_INITIALIZER(s_devs);

bool mgos_vfs_dev_register_type(const char *type,
                                const struct mgos_vfs_dev_ops *ops) {
  if (ops->open == NULL || ops->read == NULL || ops->write == NULL ||
      ops->erase == NULL || ops->get_size == NULL || ops->close == NULL) {
    /* All methods must be implemented, even if with dummy functions. */
    LOG(LL_ERROR, ("%s: not all methods are implemented", type));
    abort();
  }
  struct mgos_vfs_dev_type_entry *dte =
      (struct mgos_vfs_dev_type_entry *) calloc(1, sizeof(*dte));
  if (dte == NULL) return false;
  dte->type = type;
  dte->ops = ops;
  SLIST_INSERT_HEAD(&s_dev_types, dte, next);
  return true;
}

static struct mgos_vfs_dev *mgos_vfs_dev_create_int(const char *type,
                                                    const char *opts,
                                                    const char *name) {
  struct mgos_vfs_dev *dev = NULL;
  struct mgos_vfs_dev_type_entry *dte;
  SLIST_FOREACH(dte, &s_dev_types, next) {
    if (strcmp(type, dte->type) == 0) {
      if (opts == NULL) opts = "";
      dev = (struct mgos_vfs_dev *) calloc(1, sizeof(*dev));
      dev->ops = dte->ops;
      dev->refs = 1;
      enum mgos_vfs_dev_err dres = dev->ops->open(dev, opts);
      if (dres != 0) {
        LOG(LL_ERROR, ("Dev %s %s open failed: %d", type, opts, dres));
        free(dev);
        dev = NULL;
      } else {
        if (name != NULL) {
          LOG(LL_INFO, ("%s: %s (%s), size %u", name, type, opts,
                        (unsigned int) dev->ops->get_size(dev)));
        }
      }
      return dev;
    }
  }
  LOG(LL_ERROR, ("Unknown device type %s", type));
  return NULL;
}

struct mgos_vfs_dev *mgos_vfs_dev_create(const char *type, const char *opts) {
  return mgos_vfs_dev_create_int(type, opts, NULL);
}

bool mgos_vfs_dev_register(struct mgos_vfs_dev *dev, const char *name) {
  if (dev == NULL || name == NULL || name[0] == '\0') return false;
  struct mgos_vfs_dev *d;
  SLIST_FOREACH(d, &s_devs, next) {
    if (d == dev || strcmp(d->name, name) == 0) {
      LOG(LL_ERROR, ("Dev %s already exists", name));
      return false;
    }
  }
  dev->name = strdup(name);
  dev->refs++;
  SLIST_INSERT_HEAD(&s_devs, dev, next);
  return true;
}

bool mgos_vfs_dev_create_and_register(const char *type, const char *opts,
                                      const char *name) {
  if (name == NULL) return false;
  struct mgos_vfs_dev *dev = mgos_vfs_dev_create_int(type, opts, name);
  if (dev == NULL) return false;
  bool res = mgos_vfs_dev_register(dev, name);
  mgos_vfs_dev_close(dev);
  return res;
}

static struct mgos_vfs_dev *find_dev(const char *name) {
  struct mgos_vfs_dev *dev;
  if (name == NULL) return false;
  SLIST_FOREACH(dev, &s_devs, next) {
    if (strcmp(dev->name, name) == 0) break;
  }
  return dev;
}

struct mgos_vfs_dev *mgos_vfs_dev_open(const char *name) {
  struct mgos_vfs_dev *dev = find_dev(name);
  if (dev != NULL) {
    dev->refs++;
  } else {
    LOG(LL_ERROR, ("No such device %s", name));
  }
  return dev;
}

bool mgos_vfs_dev_close(struct mgos_vfs_dev *dev) {
  bool ret = false;
  if (dev == NULL) goto out;
  dev->refs--;
  LOG(LL_DEBUG, ("%s refs %d", (dev->name ? dev->name : ""), dev->refs));
  if (dev->refs <= 0) {
    if (dev->name != NULL) {
      SLIST_REMOVE(&s_devs, dev, mgos_vfs_dev, next);
      free(dev->name);
      dev->name = NULL;
    }
    ret = (dev->ops->close(dev) == MGOS_VFS_DEV_ERR_NONE);
    memset(dev, 0, sizeof(*dev));
    free(dev);
  }
out:
  return ret;
}

bool mgos_vfs_dev_unregister(const char *name) {
  struct mgos_vfs_dev *dev = find_dev(name);
  if (dev == NULL) return false;
  if (dev->refs > 1) {
    /* This dev is still alive, just remove the name. */
    SLIST_REMOVE(&s_devs, dev, mgos_vfs_dev, next);
    free(dev->name);
    dev->name = NULL;
  } else {
    mgos_vfs_dev_close(dev);
  }
  return true;
}

bool mgos_vfs_dev_unregister_all(void) {
  struct mgos_vfs_dev *dev, *devt;
  SLIST_FOREACH_SAFE(dev, &s_devs, next, devt) {
    mgos_vfs_dev_unregister(dev->name);
  }
  return true;
}

static char *next_field(char **stringp, const char *delim) {
  char *ret = *stringp, *p = *stringp;
  if (p != NULL) {
    *stringp = NULL;
    for (; *p != '\0' && *stringp == NULL; p++) {
      while (strchr(delim, *p) != NULL) {
        *p = '\0';
        *stringp = ++p;
      }
    }
  }
  return ret;
}

static bool mgos_process_devtab_entry(char *e) {
  bool res = false;
  const char *e_orig = e;
  const char *name = next_field(&e, " \t");
  const char *type = next_field(&e, " \t");
  const char *opts = e;
  if (name == NULL || type == NULL) {
    LOG(LL_ERROR, ("Invalid devtab entry '%s'", e_orig));
    goto out;
  }
  if (opts == NULL) opts = "";
  res = mgos_vfs_dev_create_and_register(type, opts, name);
out:
  return res;
}

bool mgos_process_devtab(const char *dt) {
  bool res = true;
  char *dtc = strdup(dt), *s = dtc, *e;
  while (res && (e = next_field(&s, "|\r\n")) != NULL) {
    struct mg_str es = mg_strstrip(mg_mk_str(e));
    *((char *) es.p + es.len) = '\0';
    if (es.len == 0 || *es.p == '#') continue;
    res = mgos_process_devtab_entry((char *) es.p);
  }
  free(dtc);
  return res;
}

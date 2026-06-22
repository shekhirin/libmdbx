/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Public C API surface audit for the additive async executor API.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_API_NAMES 768

struct name_list {
  char *items[MAX_API_NAMES];
  size_t count;
};

struct exempt_api {
  const char *name;
  const char *reason;
};

static const struct exempt_api exempt_apis[] = {
    {"mdbx_assert_fail", "assertion hook"},
    {"mdbx_default_pagesize", "pure helper"},
    {"mdbx_double_from_key", "pure key conversion"},
    {"mdbx_dump_val", "debug formatting helper"},
    {"mdbx_env_chk_encount_problem", "integrity-check callback helper"},
    {"mdbx_env_get_async_read_stats", "internal async-read migration diagnostic"},
    {"mdbx_env_resurrect_after_fork", "post-fork recovery helper"},
    {"mdbx_float_from_key", "pure key conversion"},
    {"mdbx_get_datacmp", "pure comparator lookup"},
    {"mdbx_get_keycmp", "pure comparator lookup"},
    {"mdbx_get_sysraminfo", "system information helper"},
    {"mdbx_int32_from_key", "pure key conversion"},
    {"mdbx_int64_from_key", "pure key conversion"},
    {"mdbx_is_readahead_reasonable", "pure helper"},
    {"mdbx_jsonInteger_from_key", "pure key conversion"},
    {"mdbx_key_from_double", "pure key conversion"},
    {"mdbx_key_from_float", "pure key conversion"},
    {"mdbx_key_from_jsonInteger", "pure key conversion"},
    {"mdbx_key_from_ptrdouble", "pure key conversion"},
    {"mdbx_key_from_ptrfloat", "pure key conversion"},
    {"mdbx_liberr2str", "error string helper"},
    {"mdbx_limits_dbsize_max", "pure limits helper"},
    {"mdbx_limits_dbsize_min", "pure limits helper"},
    {"mdbx_limits_keysize_max", "pure limits helper"},
    {"mdbx_limits_keysize_min", "pure limits helper"},
    {"mdbx_limits_pairsize4page_max", "pure limits helper"},
    {"mdbx_limits_txnsize_max", "pure limits helper"},
    {"mdbx_limits_valsize4page_max", "pure limits helper"},
    {"mdbx_limits_valsize_max", "pure limits helper"},
    {"mdbx_limits_valsize_min", "pure limits helper"},
    {"mdbx_module_handler", "process module helper"},
    {"mdbx_ratio2digits", "formatting helper"},
    {"mdbx_ratio2percents", "formatting helper"},
    {"mdbx_set_panic", "global callback setup"},
    {"mdbx_setup_debug", "global debug setup"},
    {"mdbx_setup_debug_nofmt", "global debug setup"},
    {"mdbx_strerror", "error string helper"},
    {"mdbx_strerror_ANSI2OEM", "error string helper"},
    {"mdbx_strerror_r", "error string helper"},
    {"mdbx_strerror_r_ANSI2OEM", "error string helper"}};

static void free_names(struct name_list *list) {
  for (size_t i = 0; i < list->count; ++i)
    free(list->items[i]);
  list->count = 0;
}

static bool list_contains(const struct name_list *list, const char *name) {
  for (size_t i = 0; i < list->count; ++i)
    if (strcmp(list->items[i], name) == 0)
      return true;
  return false;
}

static char *dup_range(const char *begin, const char *end) {
  const size_t bytes = (size_t)(end - begin);
  char *const result = malloc(bytes + 1);
  if (!result)
    return NULL;
  memcpy(result, begin, bytes);
  result[bytes] = '\0';
  return result;
}

static int list_add_unique(struct name_list *list, const char *begin, const char *end) {
  if (list->count >= MAX_API_NAMES)
    return -1;
  char *const name = dup_range(begin, end);
  if (!name)
    return -1;
  if (list_contains(list, name)) {
    free(name);
    return 0;
  }
  list->items[list->count++] = name;
  return 0;
}

static bool name_is_exempt(const char *name) {
  for (size_t i = 0; i < sizeof(exempt_apis) / sizeof(exempt_apis[0]); ++i)
    if (strcmp(exempt_apis[i].name, name) == 0)
      return true;
  return false;
}

static char *read_file(const char *path, size_t *bytes) {
  FILE *const file = fopen(path, "rb");
  if (!file)
    return NULL;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  const long size = ftell(file);
  if (size < 0) {
    fclose(file);
    return NULL;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  char *const buffer = malloc((size_t)size + 1);
  if (!buffer) {
    fclose(file);
    return NULL;
  }
  const size_t read_bytes = fread(buffer, 1, (size_t)size, file);
  fclose(file);
  if (read_bytes != (size_t)size) {
    free(buffer);
    return NULL;
  }
  buffer[read_bytes] = '\0';
  *bytes = read_bytes;
  return buffer;
}

static bool is_name_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

static const char *extract_mdbx_name(const char *begin, const char *end, const char **name_end) {
  const char needle[] = "mdbx_";
  const size_t needle_len = sizeof(needle) - 1;
  for (const char *p = begin; p + needle_len < end; ++p) {
    if (memcmp(p, needle, needle_len) != 0)
      continue;
    const char *q = p + needle_len;
    while (q < end && is_name_char(*q))
      ++q;
    const char *r = q;
    while (r < end && (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n'))
      ++r;
    if (r < end && *r == '(') {
      *name_end = q;
      return p;
    }
  }
  return NULL;
}

static int add_async_mapping(struct name_list *async_map, const char *name, const char *name_end) {
  const char async_prefix[] = "mdbx_async_";
  const size_t async_prefix_len = sizeof(async_prefix) - 1;
  const size_t suffix_len = (size_t)(name_end - name - async_prefix_len);
  char *const full = malloc(suffix_len + 6);
  if (!full)
    return -1;
  strcpy(full, "mdbx_");
  memcpy(full + 5, name + async_prefix_len, suffix_len);
  full[suffix_len + 5] = '\0';
  if (!list_contains(async_map, full)) {
    if (async_map->count >= MAX_API_NAMES) {
      free(full);
      return -1;
    }
    async_map->items[async_map->count++] = full;
  } else {
    free(full);
  }
  return 0;
}

static int collect_public_apis(const char *header, struct name_list *blocking, struct name_list *async_declared,
                               struct name_list *async_map) {
  const char marker[] = "LIBMDBX_API";
  const size_t marker_len = sizeof(marker) - 1;
  const char *p = header;
  while ((p = strstr(p, marker)) != NULL) {
    const char *const statement_end = strchr(p, ';');
    if (!statement_end)
      break;
    const char *name_end = NULL;
    const char *const name = extract_mdbx_name(p + marker_len, statement_end, &name_end);
    if (name) {
      const char async_prefix[] = "mdbx_async_";
      const size_t async_prefix_len = sizeof(async_prefix) - 1;
      if ((size_t)(name_end - name) > async_prefix_len && memcmp(name, async_prefix, async_prefix_len) == 0) {
        if (list_add_unique(async_declared, name, name_end) != 0 || add_async_mapping(async_map, name, name_end) != 0)
          return -1;
      } else if (list_add_unique(blocking, name, name_end) != 0) {
        return -1;
      }
    }
    p = statement_end + 1;
  }
  return 0;
}

static const char *skip_space(const char *p, const char *end) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
    ++p;
  return p;
}

static const char *find_matching_paren(const char *open, const char *end) {
  int depth = 0;
  for (const char *p = open; p < end; ++p) {
    if (*p == '(') {
      depth += 1;
    } else if (*p == ')') {
      depth -= 1;
      if (depth == 0)
        return p;
    }
  }
  return NULL;
}

static bool source_has_function_definition(const char *source, size_t source_bytes, const char *name) {
  const char *const end = source + source_bytes;
  const size_t name_len = strlen(name);
  const char *p = source;
  while ((p = strstr(p, name)) != NULL) {
    const bool left_ok = p == source || !is_name_char(p[-1]);
    const bool right_ok = p + name_len >= end || !is_name_char(p[name_len]);
    if (left_ok && right_ok) {
      const char *const args = skip_space(p + name_len, end);
      if (args < end && *args == '(') {
        const char *const close = find_matching_paren(args, end);
        if (close) {
          const char *const after = skip_space(close + 1, end);
          if (after < end && *after == '{')
            return true;
        }
      }
    }
    p += name_len;
  }
  return false;
}

int main(int argc, char **argv) {
  const char *const header_path = argc > 1 ? argv[1] : "mdbx.h";
  const char *const source_path = argc > 2 ? argv[2] : NULL;
  size_t header_bytes = 0;
  char *const header = read_file(header_path, &header_bytes);
  if (!header) {
    fprintf(stderr, "async-api-audit: unable to read %s\n", header_path);
    return 2;
  }

  struct name_list blocking = {{0}, 0};
  struct name_list async_declared = {{0}, 0};
  struct name_list async_map = {{0}, 0};
  const int collect_rc = collect_public_apis(header, &blocking, &async_declared, &async_map);
  free(header);
  if (collect_rc != 0) {
    fprintf(stderr, "async-api-audit: failed to collect API names\n");
    free_names(&blocking);
    free_names(&async_declared);
    free_names(&async_map);
    return 2;
  }

  size_t covered = 0;
  size_t exempt = 0;
  size_t missing = 0;
  for (size_t i = 0; i < blocking.count; ++i) {
    const char *const name = blocking.items[i];
    if (list_contains(&async_map, name)) {
      covered += 1;
    } else if (name_is_exempt(name)) {
      exempt += 1;
    } else {
      fprintf(stderr, "async-api-audit: missing async counterpart or exemption for %s\n", name);
      missing += 1;
    }
  }

  size_t async_only = 0;
  for (size_t i = 0; i < async_map.count; ++i)
    if (!list_contains(&blocking, async_map.items[i]))
      async_only += 1;

  size_t unimplemented = 0;
  if (source_path) {
    size_t source_bytes = 0;
    char *const source = read_file(source_path, &source_bytes);
    if (!source) {
      fprintf(stderr, "async-api-audit: unable to read %s\n", source_path);
      free_names(&blocking);
      free_names(&async_declared);
      free_names(&async_map);
      return 2;
    }
    for (size_t i = 0; i < async_declared.count; ++i) {
      const char *const name = async_declared.items[i];
      if (!source_has_function_definition(source, source_bytes, name)) {
        fprintf(stderr, "async-api-audit: missing implementation for %s\n", name);
        unimplemented += 1;
      }
    }
    free(source);
  }

  printf("async-api-audit: blocking=%zu async-declared=%zu async-covered=%zu async-only=%zu exempt=%zu missing=%zu",
         blocking.count, async_declared.count, covered, async_only, exempt, missing);
  if (source_path)
    printf(" unimplemented=%zu", unimplemented);
  printf("\n");

  free_names(&blocking);
  free_names(&async_declared);
  free_names(&async_map);
  return missing == 0 && unimplemented == 0 ? 0 : 1;
}

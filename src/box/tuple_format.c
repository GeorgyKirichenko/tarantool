/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "json/path.h"
#include "tuple_format.h"
#include "assoc.h"

/** Global table of tuple formats */
struct tuple_format **tuple_formats;
static intptr_t recycled_format_ids = FORMAT_ID_NIL;

static uint32_t formats_size = 0, formats_capacity = 0;

static const struct tuple_field tuple_field_default = {
	FIELD_TYPE_ANY, TUPLE_OFFSET_SLOT_NIL, false, false, {{NULL, 0}}
};

/**
 * Propagate @a field to MessagePack(field)[key].
 * @param[in][out] field Field to propagate.
 * @param key Key to propagate to.
 * @param len Length of @a key.
 * @param field_idx Field index in map.
 *
 * @retval  0 Success, the index was found.
 * @retval -1 Not found.
 */
static inline int
tuple_field_go_to_key(const char **field, const char *key, int len,
		      uint32_t *field_idx)
{
	enum mp_type type = mp_typeof(**field);
	if (type != MP_MAP)
		return -1;
	uint32_t count = mp_decode_map(field);
	for (uint32_t idx = 0; idx < count; idx++) {
		type = mp_typeof(**field);
		if (type == MP_STR) {
			uint32_t value_len;
			const char *value = mp_decode_str(field, &value_len);
			if (value_len == (uint)len &&
			    memcmp(value, key, len) == 0) {
				*field_idx = idx;
				return 0;
			}
		} else {
			/* Skip key. */
			mp_next(field);
		}
		/* Skip value. */
		mp_next(field);
	}
	return -1;
}

struct mh_strnptr_node_t *
json_path_hash_get(struct mh_strnptr_t *hashtable, const char *path,
		   uint32_t path_len, uint32_t path_hash)
{
	assert(hashtable != NULL);
	struct mh_strnptr_key_t key = {path, path_len, path_hash};
	mh_int_t rc = mh_strnptr_find(hashtable, &key, NULL);
	if (rc == mh_end(hashtable))
		return NULL;
	return mh_strnptr_node(hashtable, rc);
}

/**
 * Create a new hashtable object.
 * @param[out] hashtable Pointer to object to create.
 * @param records Count of records to reserve.
 * @retval -1 On error.
 * @retval 0 On success.
 */
static struct mh_strnptr_t *
json_path_hash_create(uint32_t records)
{
	struct mh_strnptr_t *ret = mh_strnptr_new();
	if (ret == NULL) {
		diag_set(OutOfMemory, sizeof(struct mh_strnptr_t),
			"mh_strnptr_new", "hashtable");
		return NULL;
	}
	if (records > 0 &&
	    mh_strnptr_reserve(ret, records, NULL) != 0) {
		mh_strnptr_delete(ret);
		diag_set(OutOfMemory, records, "mh_strnptr_reserve",
			 "hashtable");
		return NULL;
	}
	return ret;
}

/**
 * Delete @hashtable object.
 * @param hashtable Pointer to object to delete.
 */
static void
json_path_hash_delete(struct mh_strnptr_t *hashtable)
{
	assert(hashtable != NULL);
	while (mh_size(hashtable) != 0) {
		mh_int_t n = mh_first(hashtable);
		mh_strnptr_del(hashtable, n, NULL);
	}
	mh_strnptr_delete(hashtable);
}

/**
 * Insert a new record to hashtable.
 * @param hashtable Storage to insert new record.
 * @param path String with path.
 * @param path_len Length of @path.
 * @param field Value to store in @hashtable.
 * @retval -1 On error.
 * @retval 0 On success.
 */
static int
json_path_hash_insert(struct mh_strnptr_t *hashtable, const char *path,
		      uint32_t path_len, struct tuple_field *field)
{
	assert(hashtable != NULL);
	uint32_t path_hash = mh_strn_hash(path, path_len);
	struct mh_strnptr_node_t name_node = {path, path_len, path_hash, field};
	mh_int_t rc = mh_strnptr_put(hashtable, &name_node, NULL, NULL);
	if (rc == mh_end(hashtable)) {
		diag_set(OutOfMemory, sizeof(*hashtable), "mh_strnptr_put",
			"hashtable");
		return -1;
	}
	return 0;
}

/**
 * Construct field tree level for JSON path part.
 *
 * @param[in, out] tuple_field Pointer to record to start with
 *                 would be changed to record that math
 *                 @part lexeme.
 * @param fieldno Number of root space field.
 * @param part JSON path lexeme to represent in field tree.
 * @retval -1 On error.
 * @retval 0 On success.
 */
static int
json_field_tree_append(struct tuple_field **field_subtree, uint32_t fieldno,
		       struct json_path_node *part)
{
	enum field_type type;
	struct tuple_field *field = *field_subtree;
	switch (part->type) {
	case JSON_PATH_NUM: {
		type = FIELD_TYPE_ARRAY;
		if (field->type != FIELD_TYPE_ANY && field->type != type)
			goto error_type_mistmatch;
		/* Create or resize field array if required. */
		if (field->array == NULL || part->num > field->array_size) {
			struct tuple_field **array =
				realloc(field->array,
					part->num * sizeof(array[0]));
			if (array == NULL) {
				diag_set(OutOfMemory,
					 part->num * sizeof(array[0]),
					 "realloc","array");
				return -1;
			}
			memset(&array[field->array_size], 0,
				(part->num - field->array_size) *
				sizeof(array[0]));
			field->array = array;
			field->array_size = part->num;
			field->type = type;
		} else if (field->array[part->num - TUPLE_INDEX_BASE] != NULL) {
			/* Record already exists. No actions required */
			*field_subtree =
				field->array[part->num - TUPLE_INDEX_BASE];
			return 0;
		}
		break;
	}
	case JSON_PATH_STR: {
		type = FIELD_TYPE_MAP;
		if (field->type != FIELD_TYPE_ANY && field->type != type)
			goto error_type_mistmatch;
		if (field->map == NULL) {
			field->map = json_path_hash_create(1);
			if (field->map == NULL)
				return -1;
			field->type = type;
		} else {
			uint32_t str_hash = mh_strn_hash(part->str, part->len);
			struct mh_strnptr_node_t *ht_record =
				json_path_hash_get(field->map, part->str,
						   part->len, str_hash);
			if (ht_record != NULL) {
				assert(ht_record->val != NULL);
				*field_subtree = ht_record->val;
				return 0;
			}
		}
		break;
	}
	default:
		unreachable();
	}

	/* Construct and insert a new record. */
	struct tuple_field *new_field = malloc(sizeof(struct tuple_field));
	if (new_field == NULL) {
		diag_set(OutOfMemory, sizeof(struct tuple_field), "malloc",
			"new_field");
		return -1;
	}
	*new_field = tuple_field_default;
	if (field->type == FIELD_TYPE_MAP) {
		if (json_path_hash_insert(field->map, part->str, part->len,
					  new_field) != 0) {
			free(new_field);
			return -1;
		}
	} else if (field->type == FIELD_TYPE_ARRAY) {
		field->array[part->num - TUPLE_INDEX_BASE] = new_field;
	}
	*field_subtree = new_field;
	return 0;

error_type_mistmatch:
	diag_set(ClientError, ER_INDEX_PART_TYPE_MISMATCH,
		tt_sprintf("%d", fieldno + TUPLE_INDEX_BASE),
		field_type_strs[type], field_type_strs[field->type]);
	return -1;
}

/**
 * Delete @field_subtree object.
 * @param field_subtree To delete.
 */
static void
json_field_tree_delete(struct tuple_field *field_subtree)
{
	if (field_subtree->type == FIELD_TYPE_MAP &&
	    field_subtree->map != NULL) {
		mh_int_t i;
		mh_foreach(field_subtree->map, i) {
			struct tuple_field *field =
				mh_strnptr_node(field_subtree->map, i)->val;
			assert(field != NULL);
			json_field_tree_delete(field);
			free(field);
		}
		json_path_hash_delete(field_subtree->map);
	} else if (field_subtree->type == FIELD_TYPE_ARRAY &&
		   field_subtree->array != NULL) {
		for (uint32_t i = 0; i < field_subtree->array_size; i++) {
			struct tuple_field *field = field_subtree->array[i];
			if (field == NULL)
				continue;
			json_field_tree_delete(field_subtree->array[i]);
			free(field_subtree->array[i]);
		}
		free(field_subtree->array);
	}
}

int
tuple_field_bypass_and_init(const struct tuple_field *field, uint32_t idx,
			    const char *tuple, const char **offset,
			    uint32_t *field_map)
{
	assert(offset != NULL);
	const char *mp_data = *offset;
	const char *valid_type_str = NULL;
	const char *err = NULL;
	enum mp_type type = mp_typeof(**offset);
	if (field->type == FIELD_TYPE_MAP) {
		if (type != MP_MAP) {
			valid_type_str = mp_type_strs[MP_MAP];
			goto error_type_mistmatch;
		}
		const char *max_offset = *offset;
		uint32_t max_idx = 0;
		uint32_t count = mp_decode_map(&max_offset);
		mh_int_t i;
		mh_foreach(field->map, i) {
			struct mh_strnptr_node_t *ht_record =
				mh_strnptr_node(field->map, i);
			struct tuple_field *leaf = ht_record->val;
			assert(leaf != NULL);

			const char *raw = *offset;
			uint32_t map_idx = 0;
			int rc = tuple_field_go_to_key(&raw, ht_record->str,
						       (int)ht_record->len,
						       &map_idx);
			if (rc != 0 && !leaf->is_nullable) {
				err = tt_sprintf("map doesn't contain key "
						 "'%.*s' defined in index",
						 ht_record->len,ht_record->str);
				goto error_invalid_document;
			}
			if (rc != 0) {
				if (field_map != NULL &&
				    leaf->offset_slot != TUPLE_OFFSET_SLOT_NIL)
				    field_map[leaf->offset_slot] = 0;
				continue;
			}
			if (tuple_field_bypass_and_init(leaf, idx, tuple, &raw,
							field_map) != 0)
				return -1;
			max_idx = MAX(max_idx, map_idx + 1);
			max_offset = MAX(max_offset, raw);
		}
		*offset = max_offset;
		while (count-- > max_idx) {
			mp_next(offset);
			mp_next(offset);
		}
		return 0;
	} else if (field->type == FIELD_TYPE_ARRAY) {
		if (type != MP_ARRAY) {
			valid_type_str = mp_type_strs[MP_ARRAY];
			goto error_type_mistmatch;
		}
		uint32_t count = mp_decode_array(offset);
		for (uint32_t i = count; i < field->array_size; i++) {
			/*
			 * Index fields out of document array
			 * must be nullable.
			 */
			struct tuple_field *leaf = field->array[i];
			if (leaf == NULL)
				continue;
			if (leaf->is_nullable) {
				if (field_map != NULL &&
				    leaf->offset_slot != TUPLE_OFFSET_SLOT_NIL)
					field_map[leaf->offset_slot] = 0;
				continue;
			}
			err = tt_sprintf("array size %d is less than size of %d "
					 "defined in index", i, i + 1);
			goto error_invalid_document;
		}
		uint32_t fields = MIN(field->array_size, count);
		for (uint32_t i = 0; i < fields; i++) {
			if (field->array[i] == NULL) {
				mp_next(offset);
				continue;
			}
			if (tuple_field_bypass_and_init(field->array[i], idx,
							tuple,
							offset, field_map) != 0)
				return -1;
		}
		while (count-- > fields)
			mp_next(offset);
		return 0;
	}
	/* Tree leaf field */
	if (key_mp_type_validate(field->type, type, ER_KEY_PART_TYPE, idx,
				 field->is_nullable) != 0) {
		valid_type_str = field_type_strs[field->type];
		goto error_type_mistmatch;
	}
	assert(offset != NULL);
	if (field_map != NULL &&
	    field->offset_slot != TUPLE_OFFSET_SLOT_NIL)
		field_map[field->offset_slot] = (uint32_t) (*offset - tuple);
	mp_next(offset);
	return 0;

error_type_mistmatch:
	err = tt_sprintf("type mismatch: have %s, expected %s",
			 mp_type_strs[type], valid_type_str);
error_invalid_document:
	assert(err != NULL);
	char *data_buff = tt_static_buf();
	mp_snprint(data_buff, TT_STATIC_BUF_LEN, mp_data);
	const char *err_msg =
		tt_sprintf("invalid field %d document content '%s': %s",
			   idx + TUPLE_INDEX_BASE, data_buff, err);
	diag_set(ClientError, ER_DATA_STRUCTURE_MISMATCH, err_msg);
	return -1;
}

/**
 * Add new JSON @path to @format.
 * @param format Tuple format to modify.
 * @param path String to add.
 * @param path_len Length of @path.
 * @param path_hash Hash of @path.
 * @param type Type of field by @path.
 * @param is_nullable Nullability of field by @path.
 * @param strings Area to store unique JSON paths (optional).
 * @param[out] leaf Pointer to leaf field.
 * @retval -1 On error.
 * @retval 0 On success.
 */
static int
tuple_format_add_json_path(struct tuple_format *format, const char *path,
			   uint32_t path_len, uint32_t path_hash,
			   enum field_type type, bool is_nullable,
			   char **strings, struct tuple_field **leaf)
{
	assert(format->path_hash != NULL);
	/*
	 * Get root field by index.
	 * Path is specified in canonical form: [i]...
	 */
	struct json_path_parser parser;
	struct json_path_node node;
	json_path_parser_create(&parser, path, path_len);
	int rc = json_path_next(&parser, &node);
	assert(rc == 0 && node.type == JSON_PATH_NUM);
	assert(node.num - TUPLE_INDEX_BASE < format->field_count);

	/* Test if path is already registered. */
	struct mh_strnptr_node_t *ht_record =
		json_path_hash_get(format->path_hash, path, path_len, path_hash);
	assert(ht_record != NULL);
	struct tuple_field *field = ht_record->val;
	if (unlikely(field != NULL)) {
		/* Path has been already registered. */
		if (field->is_nullable != is_nullable)
			field->is_nullable = false;
		if (field_type1_contains_type2(field->type, type)) {
			field->type = type;
		} else if (!field_type1_contains_type2(type, field->type)) {
			const char *err =
			    tt_sprintf("JSON path '%.*s' has been already "
				       "constructed for '%s' leaf record",
				       path_len, path,
				       field_type_strs[field->type]);
			diag_set(ClientError, ER_WRONG_INDEX_OPTIONS,
				 node.num, err);
			return -1;
		}
		*leaf = field;
		return 0;
	} else if (strings != NULL) {
		/*
		 * Hashtable should hold memory related to format
		 * chunk allocation.
		 */
		memcpy(*strings, path, path_len);
		(*strings)[path_len] = '\0';
		ht_record->str = *strings;
		*strings += path_len + 1;
	}

	/*
	 * We have to re-init parser with path string located in
	 * format chunk.
	 */
	json_path_parser_create(&parser, ht_record->str + parser.offset,
				path_len - parser.offset);
	/* Build data path tree. */
	uint32_t root_fieldno = node.num - TUPLE_INDEX_BASE;
	field = &format->fields[root_fieldno];
	while ((rc = json_path_next(&parser, &node)) == 0 &&
		node.type != JSON_PATH_END) {
		if (json_field_tree_append(&field, root_fieldno, &node) != 0)
			return -1;
	}
	assert(rc == 0 && node.type == JSON_PATH_END);

	/* Leaf record is a new object as JSON path unique. */
	field->type = type;
	field->is_nullable = is_nullable;
	*leaf = field;
	ht_record->val = field;
	return 0;
}

/**
 * Add a new key_part to format and initialize format tuple_field
 * representation.
 * @param format Format to initialize.
 * @param fields Fields definition if any.
 * @param fields_count Count of @fields.
 * @param part An index part to append.
 * @param is_sequential Does this part sequential.
 * @param data Memory to store path strings.
 * @param current_slot Pointer to last offset slot.
 * @retval -1 On error.
 * @retval 0 On success.
 */
static int
tuple_format_add_key_part(struct tuple_format *format,
			  const struct field_def *fields, uint32_t field_count,
			  const struct key_part *part, bool is_sequential,
			  char **data, int *current_slot)
{
	assert(part->fieldno < format->field_count);
	struct tuple_field *field = &format->fields[part->fieldno];
	if (part->path != NULL) {
		field->is_key_part = true;
		assert(!is_sequential);
		struct tuple_field *leaf = NULL;
		if (tuple_format_add_json_path(format, part->path,
					       part->path_len, part->path_hash,
					       part->type, part->is_nullable,
					       data, &leaf) != 0)
			return -1;
		assert(leaf != NULL);
		if (leaf->offset_slot == TUPLE_OFFSET_SLOT_NIL) {
			*current_slot = *current_slot - 1;
			leaf->offset_slot = *current_slot;
		}
		return 0;
	}
	if (part->fieldno >= field_count) {
		field->is_nullable = part->is_nullable;
	} else if (field->is_nullable != part->is_nullable) {
		/*
		 * In case of mismatch set the most
		 * strict option for is_nullable.
		 */
		field->is_nullable = false;
	}
	/*
	 * Check that there are no conflicts
	 * between index part types and space
	 * fields. If a part type is compatible
	 * with field's one, then the part type is
	 * more strict and the part type must be
	 * used in tuple_format.
	 */
	if (field_type1_contains_type2(field->type, part->type)) {
		field->type = part->type;
	} else if (!field_type1_contains_type2(part->type, field->type)) {
		int fieldno = part->fieldno + TUPLE_INDEX_BASE;
		const char *name = part->fieldno >= field_count ?
				   tt_sprintf("%d", fieldno) :
				   tt_sprintf("'%s'",
					      fields[part->fieldno].name);
		int errcode = !field->is_key_part ?
			      ER_FORMAT_MISMATCH_INDEX_PART :
			      ER_INDEX_PART_TYPE_MISMATCH;
		diag_set(ClientError, errcode, name,
			 field_type_strs[field->type],
			 field_type_strs[part->type]);
		return -1;
	}
	field->is_key_part = true;
	/*
	 * In the tuple, store only offsets necessary
	 * to access fields of non-sequential keys.
	 * First field is always simply accessible,
	 * so we don't store an offset for it.
	 */
	if (field->offset_slot == TUPLE_OFFSET_SLOT_NIL && !is_sequential &&
	    part->fieldno > 0) {
		*current_slot = *current_slot - 1;
		field->offset_slot = *current_slot;
	}
	return 0;
}

/**
 * Extract all available type info from keys and field
 * definitions.
 */
static int
tuple_format_create(struct tuple_format *format, struct key_def * const *keys,
		    uint16_t key_count, const struct field_def *fields,
		    uint32_t field_count)
{
	format->min_field_count =
		tuple_format_min_field_count(keys, key_count, fields,
					     field_count);
	if (format->field_count == 0) {
		format->field_map_size = 0;
		return 0;
	}
	/* Initialize defined fields */
	for (uint32_t i = 0; i < field_count; ++i) {
		format->fields[i].is_key_part = false;
		format->fields[i].type = fields[i].type;
		format->fields[i].offset_slot = TUPLE_OFFSET_SLOT_NIL;
		format->fields[i].is_nullable = fields[i].is_nullable;
		/* Don't need to init format->fields[i].map. */
		format->fields[i].childs = NULL;
		format->fields[i].array_size = 0;
	}
	/* Initialize remaining fields */
	for (uint32_t i = field_count; i < format->field_count; i++)
		format->fields[i] = tuple_field_default;

	int current_slot = 0;
	/* Memory allocated for JSON paths if any. */
	char *data = (char *)format + sizeof(struct tuple_format) +
		     format->field_count * sizeof(struct tuple_field);

	/* extract field type info */
	for (uint16_t key_no = 0; key_no < key_count; ++key_no) {
		const struct key_def *key_def = keys[key_no];
		bool is_sequential = key_def_is_sequential(key_def);
		const struct key_part *part = key_def->parts;
		const struct key_part *parts_end = part + key_def->part_count;
		for (; part < parts_end; part++) {
			if (tuple_format_add_key_part(format, fields,
						      field_count, part,
						      is_sequential, &data,
						      &current_slot) != 0)
				return -1;
		}
	}

	assert(format->fields[0].offset_slot == TUPLE_OFFSET_SLOT_NIL);
	size_t field_map_size = -current_slot * sizeof(uint32_t);
	if (field_map_size + format->extra_size > UINT16_MAX) {
		/** tuple->data_offset is 16 bits */
		diag_set(ClientError, ER_INDEX_FIELD_COUNT_LIMIT,
			 -current_slot);
		return -1;
	}
	format->field_map_size = field_map_size;
	return 0;
}

static int
tuple_format_register(struct tuple_format *format)
{
	if (recycled_format_ids != FORMAT_ID_NIL) {

		format->id = (uint16_t) recycled_format_ids;
		recycled_format_ids = (intptr_t) tuple_formats[recycled_format_ids];
	} else {
		if (formats_size == formats_capacity) {
			uint32_t new_capacity = formats_capacity ?
						formats_capacity * 2 : 16;
			struct tuple_format **formats;
			formats = (struct tuple_format **)
				realloc(tuple_formats, new_capacity *
						       sizeof(tuple_formats[0]));
			if (formats == NULL) {
				diag_set(OutOfMemory,
					 sizeof(struct tuple_format), "malloc",
					 "tuple_formats");
				return -1;
			}

			formats_capacity = new_capacity;
			tuple_formats = formats;
		}
		if (formats_size == FORMAT_ID_MAX + 1) {
			diag_set(ClientError, ER_TUPLE_FORMAT_LIMIT,
				 (unsigned) formats_capacity);
			return -1;
		}
		format->id = formats_size++;
	}
	tuple_formats[format->id] = format;
	return 0;
}

static void
tuple_format_deregister(struct tuple_format *format)
{
	if (format->id == FORMAT_ID_NIL)
		return;
	tuple_formats[format->id] = (struct tuple_format *) recycled_format_ids;
	recycled_format_ids = format->id;
	format->id = FORMAT_ID_NIL;
}

static struct tuple_format *
tuple_format_alloc(struct key_def * const *keys, uint16_t key_count,
		   uint32_t space_field_count, struct tuple_dictionary *dict)
{
	uint32_t index_field_count = 0;
	/* JSON path hashtable. */
	struct mh_strnptr_t *path_hash = json_path_hash_create(0);
	if (path_hash == NULL)
		return NULL;
	/* find max max field no */
	for (uint16_t key_no = 0; key_no < key_count; ++key_no) {
		const struct key_def *key_def = keys[key_no];
		const struct key_part *part = key_def->parts;
		const struct key_part *pend = part + key_def->part_count;
		for (; part < pend; part++) {
			if (part->path != NULL &&
			    json_path_hash_insert(path_hash, part->path,
						  part->path_len, NULL) != 0)
				goto error;
			index_field_count = MAX(index_field_count,
						part->fieldno + 1);
		}
	}
	size_t extra_size = 0;
	if (mh_size(path_hash) == 0) {
		/* Hashtable is useless. */
		json_path_hash_delete(path_hash);
		path_hash = NULL;
	} else {
		/*
		 * Calculate unique JSON paths count.
		 * Path data would be copied later on
		 * tuple_format_create routine.
		 */
		mh_int_t i;
		mh_foreach(path_hash, i) {
			struct mh_strnptr_node_t *node =
				mh_strnptr_node(path_hash, i);
			extra_size += node->len + 1;
		}
	}
	uint32_t field_count = MAX(space_field_count, index_field_count);
	uint32_t total = sizeof(struct tuple_format) +
			 field_count * sizeof(struct tuple_field) + extra_size;

	struct tuple_format *format = (struct tuple_format *) malloc(total);
	if (format == NULL) {
		diag_set(OutOfMemory, sizeof(struct tuple_format), "malloc",
			 "tuple format");
		goto error;
	}
	if (dict == NULL) {
		assert(space_field_count == 0);
		format->dict = tuple_dictionary_new(NULL, 0);
		if (format->dict == NULL) {
			free(format);
			goto error;
		}
	} else {
		format->dict = dict;
		tuple_dictionary_ref(dict);
	}
	format->refs = 0;
	format->id = FORMAT_ID_NIL;
	format->field_count = field_count;
	format->index_field_count = index_field_count;
	format->exact_field_count = 0;
	format->min_field_count = 0;
	format->path_hash = path_hash;
	return format;
error:
	json_path_hash_delete(path_hash);
	return NULL;
}

/** Free tuple format resources, doesn't unregister. */
static inline void
tuple_format_destroy(struct tuple_format *format)
{
	for (uint32_t i = 0; i < format->field_count; i++)
		json_field_tree_delete(&format->fields[i]);
	if (format->path_hash != NULL)
		json_path_hash_delete(format->path_hash);
	tuple_dictionary_unref(format->dict);
}

void
tuple_format_delete(struct tuple_format *format)
{
	tuple_format_deregister(format);
	tuple_format_destroy(format);
	free(format);
}

struct tuple_format *
tuple_format_new(struct tuple_format_vtab *vtab, struct key_def * const *keys,
		 uint16_t key_count, uint16_t extra_size,
		 const struct field_def *space_fields,
		 uint32_t space_field_count, struct tuple_dictionary *dict)
{
	struct tuple_format *format =
		tuple_format_alloc(keys, key_count, space_field_count, dict);
	if (format == NULL)
		return NULL;
	format->vtab = *vtab;
	format->engine = NULL;
	format->extra_size = extra_size;
	format->is_temporary = false;
	if (tuple_format_register(format) < 0) {
		tuple_format_destroy(format);
		free(format);
		return NULL;
	}
	if (tuple_format_create(format, keys, key_count, space_fields,
				space_field_count) < 0) {
		tuple_format_delete(format);
		return NULL;
	}
	return format;
}

bool
tuple_format1_can_store_format2_tuples(const struct tuple_format *format1,
				       const struct tuple_format *format2)
{
	if (format1->exact_field_count != format2->exact_field_count)
		return false;
	for (uint32_t i = 0; i < format1->field_count; ++i) {
		const struct tuple_field *field1 = &format1->fields[i];
		/*
		 * The field has a data type in format1, but has
		 * no data type in format2.
		 */
		if (i >= format2->field_count) {
			/*
			 * The field can get a name added
			 * for it, and this doesn't require a data
			 * check.
			 * If the field is defined as not
			 * nullable, however, we need a data
			 * check, since old data may contain
			 * NULLs or miss the subject field.
			 */
			if (field1->type == FIELD_TYPE_ANY &&
			    field1->is_nullable)
				continue;
			else
				return false;
		}
		const struct tuple_field *field2 = &format2->fields[i];
		if (! field_type1_contains_type2(field1->type, field2->type))
			return false;
		/*
		 * Do not allow transition from nullable to non-nullable:
		 * it would require a check of all data in the space.
		 */
		if (field2->is_nullable && !field1->is_nullable)
			return false;
	}
	return true;
}

struct tuple_format *
tuple_format_dup(struct tuple_format *src)
{
	uint32_t total = sizeof(struct tuple_format) +
			 src->field_count * sizeof(struct tuple_field);
	if (src->path_hash != NULL) {
		mh_int_t i;
		mh_foreach(src->path_hash, i)
			total += mh_strnptr_node(src->path_hash, i)->len + 1;
	}
	struct tuple_format *format = (struct tuple_format *) malloc(total);
	if (format == NULL) {
		diag_set(OutOfMemory, total, "malloc", "tuple format");
		return NULL;
	}
	memcpy(format, src, total);

	/* Fill with NULLs for normal destruction on error. */
	format->path_hash = NULL;
	for (uint32_t i = 0; i < format->field_count; i++) {
		format->fields[i].childs = NULL;
		format->fields[i].array_size = 0;
	}
	if (src->path_hash != NULL) {
		mh_int_t i;
		format->path_hash =
			json_path_hash_create(mh_size(src->path_hash));
		if (format->path_hash == NULL)
			goto error;
		mh_foreach(src->path_hash, i) {
			struct mh_strnptr_node_t *node =
				mh_strnptr_node(src->path_hash, i);
			/* Path data has been already copied. */
			char *path = (char *)format + (node->str - (char *)src);
			if (json_path_hash_insert(format->path_hash, path,
						  node->len, NULL) != 0)
				goto error;
			/* Store source leaf field offset_slot.  */
			struct tuple_field *leaf = node->val;
			int32_t offset_slot = leaf->offset_slot;
			uint32_t path_hash = mh_strn_hash(path, node->len);
			if (tuple_format_add_json_path(format, path, node->len,
						       path_hash, leaf->type,
						       leaf->is_nullable, NULL,
						       &leaf) != 0)
				goto error;
			/* Store offset_slot in a new leaf record. */
			assert(leaf != NULL);
			leaf->offset_slot = offset_slot;
		}
	}
	tuple_dictionary_ref(format->dict);
	format->id = FORMAT_ID_NIL;
	format->refs = 0;
	if (tuple_format_register(format) == 0)
		return format;
error:
	tuple_format_destroy(format);
	free(format);
	return NULL;
}

/** @sa declaration for details. */
int
tuple_init_field_map(const struct tuple_format *format, uint32_t *field_map,
		     const char *tuple)
{
	if (format->field_count == 0)
		return 0; /* Nothing to initialize */

	const char *pos = tuple;

	/* Check to see if the tuple has a sufficient number of fields. */
	uint32_t field_count = mp_decode_array(&pos);
	if (format->exact_field_count > 0 &&
	    format->exact_field_count != field_count) {
		diag_set(ClientError, ER_EXACT_FIELD_COUNT,
			 (unsigned) field_count,
			 (unsigned) format->exact_field_count);
		return -1;
	}
	if (unlikely(field_count < format->min_field_count)) {
		diag_set(ClientError, ER_MIN_FIELD_COUNT,
			 (unsigned) field_count,
			 (unsigned) format->min_field_count);
		return -1;
	}

	uint32_t i = 0;
	enum mp_type mp_type;
	const struct tuple_field *field = &format->fields[0];
	if (field_count < format->index_field_count || field->childs != NULL) {
		/*
		 * Nullify field map to be able to detect by 0,
		 * which key fields are absent in tuple_field().
		 */
		memset((char *)field_map - format->field_map_size, 0,
		       format->field_map_size);
	}
	if (field->childs == NULL) {
		/*
		 * First field is simply accessible, do not store
		 * offset to it.
		 */
		mp_type = mp_typeof(*pos);
		if (key_mp_type_validate(field->type, mp_type, ER_FIELD_TYPE,
					 TUPLE_INDEX_BASE, field->is_nullable))
			return -1;
		mp_next(&pos);
		++field;
		++i;
	}
	uint32_t defined_field_count = MIN(field_count, format->field_count);
	for (; i < defined_field_count; ++i, ++field) {
		mp_type = mp_typeof(*pos);
		if (key_mp_type_validate(field->type, mp_type, ER_FIELD_TYPE,
					 i + TUPLE_INDEX_BASE,
					 field->is_nullable))
			return -1;
		if (field->offset_slot != TUPLE_OFFSET_SLOT_NIL) {
			field_map[field->offset_slot] =
				(uint32_t) (pos - tuple);
		} else if (field->childs != NULL &&
			   tuple_field_bypass_and_init(field, i, tuple, &pos,
						       field_map) != 0)
			return -1;
		if (field->childs == NULL)
			mp_next(&pos);
	}
	return 0;
}

uint32_t
tuple_format_min_field_count(struct key_def * const *keys, uint16_t key_count,
			     const struct field_def *space_fields,
			     uint32_t space_field_count)
{
	uint32_t min_field_count = 0;
	for (uint32_t i = 0; i < space_field_count; ++i) {
		if (! space_fields[i].is_nullable)
			min_field_count = i + 1;
	}
	for (uint32_t i = 0; i < key_count; ++i) {
		const struct key_def *kd = keys[i];
		for (uint32_t j = 0; j < kd->part_count; ++j) {
			const struct key_part *kp = &kd->parts[j];
			if (!kp->is_nullable &&
			    kp->fieldno + 1 > min_field_count)
				min_field_count = kp->fieldno + 1;
		}
	}
	return min_field_count;
}

/** Destroy tuple format subsystem and free resourses */
void
tuple_format_free()
{
	/* Clear recycled ids. */
	while (recycled_format_ids != FORMAT_ID_NIL) {
		uint16_t id = (uint16_t) recycled_format_ids;
		recycled_format_ids = (intptr_t) tuple_formats[id];
		tuple_formats[id] = NULL;
	}
	for (struct tuple_format **format = tuple_formats;
	     format < tuple_formats + formats_size; format++) {
		/* Do not unregister. Only free resources. */
		if (*format != NULL) {
			tuple_format_destroy(*format);
			free(*format);
		}
	}
	free(tuple_formats);
}

void
box_tuple_format_ref(box_tuple_format_t *format)
{
	tuple_format_ref(format);
}

void
box_tuple_format_unref(box_tuple_format_t *format)
{
	tuple_format_unref(format);
}

/**
 * Propagate @a field to MessagePack(field)[index].
 * @param[in][out] field Field to propagate.
 * @param index 1-based index to propagate to.
 *
 * @retval  0 Success, the index was found.
 * @retval -1 Not found.
 */
static inline int
tuple_field_go_to_index(const char **field, uint64_t index)
{
	enum mp_type type = mp_typeof(**field);
	if (type == MP_ARRAY) {
		if (index == 0)
			return -1;
		/* Make index 0-based. */
		index -= TUPLE_INDEX_BASE;
		uint32_t count = mp_decode_array(field);
		if (index >= count)
			return -1;
		for (; index > 0; --index)
			mp_next(field);
		return 0;
	} else if (type == MP_MAP) {
		uint64_t count = mp_decode_map(field);
		for (; count > 0; --count) {
			type = mp_typeof(**field);
			if (type == MP_UINT) {
				uint64_t value = mp_decode_uint(field);
				if (value == index)
					return 0;
			} else if (type == MP_INT) {
				int64_t value = mp_decode_int(field);
				if (value >= 0 && (uint64_t)value == index)
					return 0;
			} else {
				/* Skip key. */
				mp_next(field);
			}
			/* Skip value. */
			mp_next(field);
		}
	}
	return -1;
}

const char *
tuple_field_by_part_raw(const struct tuple_format *format, const char *data,
			const uint32_t *field_map, struct key_part *part)
{
	if (likely(part->path == NULL))
		return tuple_field_raw(format, data, field_map, part->fieldno);

	struct mh_strnptr_node_t *ht_record = NULL;
	int32_t offset_slot;
	if (format->path_hash != NULL &&
	    (ht_record = json_path_hash_get(format->path_hash, part->path,
					    part->path_len,
					    part->path_hash)) != NULL) {
		struct tuple_field *field = ht_record->val;
		assert(field != NULL);
		offset_slot = field->offset_slot;
	} else {
		/*
		 * Legacy tuple having no field map for
		 * JSON index.
		 */
		uint32_t path_hash =
			field_name_hash(part->path, part->path_len);
		const char *raw = NULL;
		if (tuple_field_raw_by_path(format, data, field_map,
					    part->path, part->path_len,
					    path_hash, &raw) != 0)
			raw = NULL;
		return raw;
	}
	assert(offset_slot < 0);
	assert(-offset_slot * sizeof(uint32_t) <= format->field_map_size);
	if (unlikely(field_map[offset_slot] == 0))
		return NULL;
	return data + field_map[offset_slot];
}

int
tuple_field_dig_with_parser(struct json_path_parser *parser, const char **field)
{
	int rc;
	struct json_path_node node;
	while ((rc = json_path_next(parser, &node)) == 0) {
		uint32_t dummy;
		switch(node.type) {
			case JSON_PATH_NUM:
				rc = tuple_field_go_to_index(field, node.num);
				break;
			case JSON_PATH_STR:
				rc = tuple_field_go_to_key(field, node.str,
							   node.len, &dummy);
				break;
			default:
				assert(node.type == JSON_PATH_END);
				return 0;
		}
		if (rc != 0) {
			*field = NULL;
			return 0;
		}
	}
	return rc;
}

int
tuple_field_raw_by_path(const struct tuple_format *format, const char *tuple,
                        const uint32_t *field_map, const char *path,
                        uint32_t path_len, uint32_t path_hash,
                        const char **field)
{
	assert(path_len > 0);
	uint32_t fieldno;
	if (format->path_hash != NULL) {
		/*
		 * The path hash for format->path_hash hashtable
		 * may may be different from path_hash specified
		 * as function argument.
		 */
		struct mh_strnptr_node_t *ht_record =
			json_path_hash_get(format->path_hash, path, path_len,
					   mh_strn_hash(path, path_len));
		if (ht_record != NULL) {
			struct tuple_field *leaf = ht_record->val;
			assert(leaf != NULL);
			int32_t offset_slot = leaf->offset_slot;
			assert(offset_slot != TUPLE_OFFSET_SLOT_NIL);
			if (likely(field_map[offset_slot] != 0))
				*field = tuple + field_map[offset_slot];
			else
				*field = NULL;
			return 0;
		}
	}
	/*
	 * It is possible, that a field has a name as
	 * well-formatted JSON. For example 'a.b.c.d' or '[1]' can
	 * be field name. To save compatibility at first try to
	 * use the path as a field name.
	 */
	if (tuple_fieldno_by_name(format->dict, path, path_len, path_hash,
				  &fieldno) == 0) {
		*field = tuple_field_raw(format, tuple, field_map, fieldno);
		return 0;
	}
	struct json_path_parser parser;
	struct json_path_node node;
	json_path_parser_create(&parser, path, path_len);
	int rc = json_path_next(&parser, &node);
	if (rc != 0)
		goto error;
	switch(node.type) {
	case JSON_PATH_NUM: {
		int index = node.num;
		if (index == 0) {
			*field = NULL;
			return 0;
		}
		index -= TUPLE_INDEX_BASE;
		*field = tuple_field_raw(format, tuple, field_map, index);
		if (*field == NULL)
			return 0;
		break;
	}
	case JSON_PATH_STR: {
		/* First part of a path is a field name. */
		uint32_t name_hash;
		if (path_len == (uint32_t) node.len) {
			name_hash = path_hash;
		} else {
			/*
			 * If a string is "field....", then its
			 * precalculated juajit hash can not be
			 * used. A tuple dictionary hashes only
			 * name, not path.
			 */
			name_hash = field_name_hash(node.str, node.len);
		}
		*field = tuple_field_raw_by_name(format, tuple, field_map,
						 node.str, node.len, name_hash);
		if (*field == NULL)
			return 0;
		break;
	}
	default:
		assert(node.type == JSON_PATH_END);
		*field = NULL;
		return 0;
	}
	rc = tuple_field_dig_with_parser(&parser, field);
	if (rc == 0)
		return 0;
error:
	assert(rc > 0);
	diag_set(ClientError, ER_ILLEGAL_PARAMS,
		 tt_sprintf("error in path on position %d", rc));
	return -1;
}

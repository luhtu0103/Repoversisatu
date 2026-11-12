#include "ai_memory.h"

#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_partition.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "AI_MEMORY";
static const char *PARTITION_LABEL = "storage";
static const char *MOUNT_PATH = "/spiffs";
static const char *MEMORY_FILE = "/spiffs/memory.json";
static bool s_initialized = false;

static bool ensure_mounted(void)
{
    if (s_initialized) return true;

    esp_vfs_spiffs_conf_t conf = {
        .base_path = MOUNT_PATH,
        .partition_label = PARTITION_LABEL,
        .max_files = 3,
        .format_if_mount_failed = false
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        const esp_partition_t *part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, PARTITION_LABEL);
        if (!part) {
            ESP_LOGE(TAG, "SPIFFS mount gagal: partisi '%s' tidak ditemukan (cek partition table yang ter-flash)",
                     PARTITION_LABEL);
            return false;
        }

        ESP_LOGE(TAG, "SPIFFS mount gagal: %s (label=%s size=%u offset=0x%lx)",
                 esp_err_to_name(err), PARTITION_LABEL, (unsigned)part->size,
                 (unsigned long)part->address);

        /*
         * The user explicitly approved resetting AI Memory storage to recover
         * from this persistent mount failure. Format only the dedicated
         * "storage" SPIFFS partition; never touch the app/model partitions.
         * On subsequent boots a valid filesystem should mount without format.
         */
        ESP_LOGW(TAG, "SPIFFS storage gagal dipasang; reset filesystem storage diizinkan untuk pemulihan AI Memory.");
        esp_err_t format_err = esp_spiffs_format(PARTITION_LABEL);
        if (format_err != ESP_OK) {
            ESP_LOGE(TAG, "Format SPIFFS gagal: %s", esp_err_to_name(format_err));
            return false;
        }

        err = esp_vfs_spiffs_register(&conf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Mount SPIFFS setelah format gagal: %s", esp_err_to_name(err));
            return false;
        }
        ESP_LOGI(TAG, "Filesystem SPIFFS baru berhasil dibuat pada partisi storage kosong.");
    }

    s_initialized = true;
    size_t total = 0, used = 0;
    esp_err_t info_err = esp_spiffs_info(PARTITION_LABEL, &total, &used);
    if (info_err == ESP_OK) {
        ESP_LOGI(TAG, "Memori lokal aktif: partisi=%s total=%u terpakai=%u bebas=%u",
                 PARTITION_LABEL, (unsigned)total, (unsigned)used,
                 (unsigned)(total - used));
    } else {
        ESP_LOGW(TAG, "SPIFFS aktif tetapi info kapasitas gagal: %s", esp_err_to_name(info_err));
    }
    return true;
}

static cJSON *load_root(void)
{
    FILE *file = fopen(MEMORY_FILE, "rb");
    if (!file) return cJSON_CreateArray();
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    long size = ftell(file);
    if (size < 0 || size > 65536 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *data = (char *)malloc((size_t)size + 1);
    if (!data) { fclose(file); return NULL; }
    size_t read_len = fread(data, 1, (size_t)size, file);
    fclose(file);
    data[read_len] = '\0';
    cJSON *root = cJSON_Parse(data);
    free(data);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "File memori tidak valid; tidak akan menimpa data rusak");
        return NULL;
    }
    return root;
}

static bool save_root(cJSON *root)
{
    if (!cJSON_IsArray(root)) return false;
    char *json = cJSON_PrintUnformatted(root);
    if (!json) return false;

    char temp_file[sizeof("/spiffs/memory.json.tmp")];
    int path_len = snprintf(temp_file, sizeof(temp_file), "%s.tmp", MEMORY_FILE);
    if (path_len < 0 || (size_t)path_len >= sizeof(temp_file)) {
        cJSON_free(json);
        return false;
    }

    FILE *file = fopen(temp_file, "wb");
    if (!file) { cJSON_free(json); return false; }
    size_t len = strlen(json);
    bool ok = fwrite(json, 1, len, file) == len && fflush(file) == 0;
    if (fclose(file) != 0) ok = false;
    cJSON_free(json);
    if (!ok) { remove(temp_file); return false; }

    /*
     * SPIFFS may reject rename(temp, existing_destination). Preserve the
     * current file as a backup first, then promote the fully-written temp.
     * If promotion fails, attempt to restore the original file.
     */
    char backup_file[sizeof("/spiffs/memory.json.bak")];
    int backup_len = snprintf(backup_file, sizeof(backup_file), "%s.bak", MEMORY_FILE);
    if (backup_len < 0 || (size_t)backup_len >= sizeof(backup_file)) {
        remove(temp_file);
        return false;
    }

    /* Recover from an interrupted previous replacement if the main file is absent. */
    FILE *main_check = fopen(MEMORY_FILE, "rb");
    bool main_exists = (main_check != NULL);
    if (main_check) fclose(main_check);
    FILE *backup_check = fopen(backup_file, "rb");
    bool backup_exists = (backup_check != NULL);
    if (backup_check) fclose(backup_check);
    if (!main_exists && backup_exists) {
        if (rename(backup_file, MEMORY_FILE) == 0) {
            main_exists = true;
            backup_exists = false;
        } else {
            remove(temp_file);
            return false;
        }
    }

    /* Remove only a stale backup when the main file is already present. */
    if (main_exists && backup_exists) {
        if (remove(backup_file) != 0) {
            remove(temp_file);
            return false;
        }
        backup_exists = false;
    }

    if (main_exists && rename(MEMORY_FILE, backup_file) != 0) {
        ESP_LOGE(TAG, "Tidak bisa memindahkan file memori lama ke backup");
        remove(temp_file);
        return false;
    }

    if (rename(temp_file, MEMORY_FILE) != 0) {
        ESP_LOGE(TAG, "Promosi file memori sementara gagal");
        if (main_exists && rename(backup_file, MEMORY_FILE) != 0)
            ESP_LOGE(TAG, "Pemulihan file memori dari backup juga gagal");
        remove(temp_file);
        return false;
    }

    if (main_exists && remove(backup_file) != 0)
        ESP_LOGW(TAG, "File memori tersimpan tetapi backup lama belum terhapus");
    return true;
}

bool ai_memory_init(void)
{
    return ensure_mounted();
}

bool ai_memory_save(const char *key, const char *value)
{
    if (!key || !value || !key[0] || !value[0]) {
        ESP_LOGE(TAG, "Simpan ditolak: key/value kosong atau null");
        return false;
    }
    if (strlen(key) > 64 || strlen(value) > 512) {
        ESP_LOGE(TAG, "Simpan ditolak: ukuran melebihi batas (key=%u/64 value=%u/512)",
                 (unsigned)strlen(key), (unsigned)strlen(value));
        return false;
    }
    if (!ensure_mounted()) {
        ESP_LOGE(TAG, "Simpan gagal: SPIFFS belum siap");
        return false;
    }
    cJSON *root = load_root();
    if (!root) {
        ESP_LOGE(TAG, "Simpan gagal: memory.json rusak/tidak dapat dibaca; data lama tidak ditimpa");
        return false;
    }
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, root) {
        cJSON *existing_key = cJSON_GetObjectItem(entry, "key");
        if (cJSON_IsString(existing_key) && strcmp(existing_key->valuestring, key) == 0) {
            cJSON *new_value = cJSON_CreateString(value);
            if (!new_value) { cJSON_Delete(root); return false; }
            cJSON_ReplaceItemInObject(entry, "value", new_value);
            bool ok = save_root(root);
            cJSON_Delete(root);
            ESP_LOGI(TAG, "Catatan diperbarui: key=%s result=%s", key, ok ? "OK" : "GAGAL");
            return ok;
        }
    }
    if (cJSON_GetArraySize(root) >= 64) {
        ESP_LOGE(TAG, "Simpan gagal: batas 64 catatan tercapai");
        cJSON_Delete(root);
        return false;
    }
    cJSON *new_entry = cJSON_CreateObject();
    if (!new_entry) {
        ESP_LOGE(TAG, "Simpan gagal: heap tidak cukup membuat entri");
        cJSON_Delete(root);
        return false;
    }
    cJSON_AddStringToObject(new_entry, "key", key);
    cJSON_AddStringToObject(new_entry, "value", value);
    cJSON_AddItemToArray(root, new_entry);
    bool ok = save_root(root);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Catatan baru: key=%s result=%s", key, ok ? "OK" : "GAGAL");
    return ok;
}

/* Case-insensitive substring search without locale-dependent libc behavior. */
static const char *find_case_insensitive(const char *text, const char *needle)
{
    if (!text || !needle || !needle[0]) return NULL;
    for (const char *p = text; *p; ++p) {
        const char *a = p, *b = needle;
        while (*a && *b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
            char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
            if (ca != cb) break;
            ++a; ++b;
        }
        if (!*b) return p;
    }
    return NULL;
}

/* Return: 1=deleted, 0=not found/storage failure, 2=ambiguous (no mutation). */
int ai_memory_delete_target(const char *target, char *detail, size_t detail_size)
{
    if (detail && detail_size) detail[0] = '\0';
    if (!target || !target[0] || !ensure_mounted()) return 0;
    cJSON *root = load_root();
    if (!root) return 0;

    /* An exact key explicitly identifies one whole record. */
    cJSON *entry = NULL;
    cJSON *exact = NULL;
    size_t exact_count = 0;
    cJSON_ArrayForEach(entry, root) {
        cJSON *k = cJSON_GetObjectItem(entry, "key");
        if (cJSON_IsString(k) && strcmp(k->valuestring, target) == 0) {
            exact = entry;
            ++exact_count;
        }
    }
    if (exact_count == 1) {
        cJSON_DetachItemViaPointer(root, exact);
        cJSON_Delete(exact);
        bool ok = save_root(root);
        cJSON_Delete(root);
        if (detail && detail_size)
            snprintf(detail, detail_size, "%s", ok ? "Catatan berhasil dihapus." : "Gagal menyimpan perubahan penghapusan.");
        ESP_LOGI(TAG, "Hapus catatan key=%s result=%s", target, ok ? "OK" : "GAGAL");
        return ok ? 1 : 0;
    }
    if (exact_count > 1) {
        cJSON_Delete(root);
        if (detail && detail_size) snprintf(detail, detail_size, "Ada beberapa catatan dengan nama yang sama; tidak ada yang dihapus. Minta pengguna memperjelas catatannya.");
        return 2;
    }

    /* Otherwise locate the sentence containing the requested phrase. Only a
       unique sentence match is allowed to mutate persistent storage. */
    cJSON *match_entry = NULL;
    const char *match_start = NULL;
    const char *match_end = NULL;
    size_t matches = 0;
    cJSON_ArrayForEach(entry, root) {
        cJSON *v = cJSON_GetObjectItem(entry, "value");
        if (!cJSON_IsString(v) || !v->valuestring) continue;
        const char *value = v->valuestring;
        const char *cursor = value;
        while (*cursor) {
            const char *hit = find_case_insensitive(cursor, target);
            if (!hit) break;
            const char *start = hit;
            while (start > value && start[-1] != '.' && start[-1] != '!' &&
                   start[-1] != '?' && start[-1] != '\n') --start;
            const char *end = hit + strlen(target);
            while (*end && *end != '.' && *end != '!' && *end != '?' && *end != '\n') ++end;
            if (*end == '.' || *end == '!' || *end == '?') ++end;
            while (*start == ' ' || *start == '\t' || *start == '\r') ++start;
            while (*end == ' ' || *end == '\t' || *end == '\r') ++end;
            match_entry = entry;
            match_start = start;
            match_end = end;
            ++matches;
            cursor = hit + strlen(target);
        }
    }
    if (matches != 1) {
        cJSON_Delete(root);
        if (detail && detail_size) {
            if (matches > 1) snprintf(detail, detail_size, "Frasa cocok dengan beberapa kalimat. Tidak ada yang dihapus; minta pengguna menyebutkan kalimat yang lebih spesifik.");
            else snprintf(detail, detail_size, "Catatan atau kalimat yang dimaksud tidak ditemukan. Tidak ada yang dihapus.");
        }
        return matches > 1 ? 2 : 0;
    }

    cJSON *value_item = cJSON_GetObjectItem(match_entry, "value");
    const char *old_value = value_item->valuestring;
    size_t prefix_len = (size_t)(match_start - old_value);
    size_t suffix_len = strlen(match_end);
    /* Remove surrounding duplicate whitespace/newlines, preserving other text. */
    while (prefix_len > 0 && (old_value[prefix_len - 1] == ' ' ||
           old_value[prefix_len - 1] == '\t' || old_value[prefix_len - 1] == '\n')) --prefix_len;
    while (*match_end == ' ' || *match_end == '\t' || *match_end == '\n') { ++match_end; suffix_len = strlen(match_end); }
    char *new_value = (char *)malloc(prefix_len + suffix_len + 2);
    if (!new_value) { cJSON_Delete(root); return 0; }
    memcpy(new_value, old_value, prefix_len);
    size_t used = prefix_len;
    if (prefix_len && suffix_len) new_value[used++] = ' ';
    memcpy(new_value + used, match_end, suffix_len);
    used += suffix_len;
    while (used > 0 && new_value[used - 1] == ' ') --used;
    new_value[used] = '\0';

    bool ok = false;
    if (used == 0) {
        cJSON_DetachItemViaPointer(root, match_entry);
        cJSON_Delete(match_entry);
        ok = save_root(root);
    } else {
        cJSON *new_item = cJSON_CreateString(new_value);
        if (new_item) {
            cJSON_ReplaceItemInObject(match_entry, "value", new_item);
            ok = save_root(root);
        }
    }
    free(new_value);
    cJSON_Delete(root);
    if (detail && detail_size)
        snprintf(detail, detail_size, "%s", ok ? "Kalimat yang cocok berhasil dihapus; kalimat lain tetap dipertahankan." : "Gagal menyimpan perubahan penghapusan.");
    ESP_LOGI(TAG, "Hapus kalimat target=%s result=%s", target, ok ? "OK" : "GAGAL");
    return ok ? 1 : 0;
}

size_t ai_memory_get_context(char *out, size_t out_size)
{
    if (!out || out_size == 0 || !ensure_mounted()) return 0;
    out[0] = '\0';
    cJSON *root = load_root();
    if (!root) return 0;
    size_t used = 0;
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, root) {
        cJSON *key = cJSON_GetObjectItem(entry, "key");
        cJSON *value = cJSON_GetObjectItem(entry, "value");
        if (!cJSON_IsString(key) || !cJSON_IsString(value)) continue;
        int written = snprintf(out + used, out_size - used, "- %s: %s\n",
                               key->valuestring, value->valuestring);
        if (written < 0 || (size_t)written >= out_size - used) break;
        used += (size_t)written;
    }
    cJSON_Delete(root);
    return used;
}

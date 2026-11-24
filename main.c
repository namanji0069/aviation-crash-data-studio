#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_LINE 8192
#define MAX_COLS 32
#define MAX_FIELD 2048

/* Exact 0-based indices for Airplane_Crashes_and_Fatalities_Since_1908_20190820105639.csv */
#define COL_DATE    0
#define COL_LOC     2
#define COL_OP      3
#define COL_AC      6
#define COL_ABOARD  9
#define COL_FATAL   12
#define COL_GROUND  15

typedef struct {
    int line_no;
    int year;
    char date[32];
    char location[160];
    char operator_name[160];
    char aircraft[160];
    int aboard;     /* -1 if missing/invalid */
    int fatalities; /* -1 if missing/invalid */
    int ground;     /* -1 if missing/invalid */
} CrashRecord;

typedef struct {
    char label[160];
    int crashes;
    long long fatalities;
} SummaryItem;

static void strip_newline(char *s) {
    s[strcspn(s, "\r\n")] = '\0';
}

static void trim_spaces(char *s) {
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    size_t start = 0;
    while (s[start] && isspace((unsigned char)s[start])) {
        start++;
    }
    if (start > 0) {
        memmove(s, s + start, strlen(s + start) + 1);
    }
}

static void clear_stdin_line(void) {
    int ch;
    while ((ch = getchar()) != '\n' && ch != EOF) {
    }
}

static int parse_csv_line(const char *line, char fields[][MAX_FIELD], int max_cols) {
    int col = 0;
    int pos = 0;
    int in_quotes = 0;

    for (size_t i = 0; line[i] != '\0'; i++) {
        char c = line[i];
        if (c == '"') {
            if (in_quotes && line[i + 1] == '"') {
                if (pos < MAX_FIELD - 1) fields[col][pos++] = '"';
                i++;
            } else {
                in_quotes = !in_quotes;
            }
        } else if (c == ',' && !in_quotes) {
            fields[col][pos] = '\0';
            trim_spaces(fields[col]);
            col++;
            pos = 0;
            if (col >= max_cols) break;
        } else {
            if (pos < MAX_FIELD - 1) {
                fields[col][pos++] = c;
            }
        }
    }
    if (col < max_cols) {
        fields[col][pos] = '\0';
        trim_spaces(fields[col]);
        col++;
    }
    return col;
}

static int parse_non_neg_int(const char *s) {
    if (!s || s[0] == '\0') return -1;
    char *end = NULL;
    double d = strtod(s, &end);
    while (end && *end && isspace((unsigned char)*end)) end++;
    if (!end || *end != '\0' || d < 0.0) return -1;
    return (int)(d + 0.5);
}

static int extract_year(const char *date_str) {
    int len = (int)strlen(date_str);
    for (int i = 0; i <= len - 4; i++) {
        if (isdigit((unsigned char)date_str[i]) &&
            isdigit((unsigned char)date_str[i + 1]) &&
            isdigit((unsigned char)date_str[i + 2]) &&
            isdigit((unsigned char)date_str[i + 3])) {
            int y = atoi(date_str + i);
            if (y >= 1900 && y <= 2026) return y;
        }
    }
    return -1;
}

static int contains_case_insensitive(const char *haystack, const char *needle) {
    if (!haystack || !needle || needle[0] == '\0') return 0;
    size_t n_len = strlen(needle);
    for (size_t i = 0; haystack[i] != '\0'; i++) {
        size_t j = 0;
        while (j < n_len && haystack[i + j] != '\0' &&
               tolower((unsigned char)haystack[i + j]) == tolower((unsigned char)needle[j])) {
            j++;
        }
        if (j == n_len) return 1;
    }
    return 0;
}

static int is_military_operator(const char *op) {
    if (!op || op[0] == '\0') return 0;
    return contains_case_insensitive(op, "military") ||
           contains_case_insensitive(op, "air force") ||
           contains_case_insensitive(op, "u.s. navy") ||
           contains_case_insensitive(op, "u.s. army") ||
           contains_case_insensitive(op, "royal air force") ||
           contains_case_insensitive(op, "indian air force");
}

static void csv_escape(const char *src, char *dst, size_t dst_size) {
    int needs_quotes = 0;
    for (size_t i = 0; src[i] != '\0'; i++) {
        if (src[i] == ',' || src[i] == '"' || src[i] == '\n') {
            needs_quotes = 1;
            break;
        }
    }
    if (!needs_quotes) {
        snprintf(dst, dst_size, "%s", src);
        return;
    }
    size_t pos = 0;
    if (pos + 1 < dst_size) dst[pos++] = '"';
    for (size_t i = 0; src[i] != '\0' && pos + 2 < dst_size; i++) {
        if (src[i] == '"') dst[pos++] = '"';
        dst[pos++] = src[i];
    }
    if (pos + 1 < dst_size) dst[pos++] = '"';
    dst[pos] = '\0';
}

static void json_escape(const char *src, char *dst, size_t dst_size) {
    size_t pos = 0;
    for (size_t i = 0; src[i] != '\0' && pos + 6 < dst_size; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            dst[pos++] = '\\';
            dst[pos++] = (char)c;
        } else if (c == '\n') {
            dst[pos++] = '\\';
            dst[pos++] = 'n';
        } else if (c == '\r') {
            dst[pos++] = '\\';
            dst[pos++] = 'r';
        } else if (c == '\t') {
            dst[pos++] = '\\';
            dst[pos++] = 't';
        } else if (c >= 32) {
            dst[pos++] = (char)c;
        }
    }
    dst[pos] = '\0';
}

CrashRecord *load_records(const char *filename, size_t *out_count) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        perror("Failed to open CSV");
        return NULL;
    }

    CrashRecord *records = NULL;
    size_t count = 0, capacity = 0;
    char line[MAX_LINE];
    char fields[MAX_COLS][MAX_FIELD];
    int line_no = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_no++;
        strip_newline(line);
        if (line[0] == '\0') continue;
        if (line_no == 1) continue; /* Skip header */

        int n_cols = parse_csv_line(line, fields, MAX_COLS);
        if (n_cols <= COL_FATAL) continue;

        if (count == capacity) {
            size_t new_cap = (capacity == 0) ? 512 : capacity * 2;
            CrashRecord *tmp = realloc(records, new_cap * sizeof(CrashRecord));
            if (!tmp) {
                free(records);
                fclose(fp);
                return NULL;
            }
            records = tmp;
            capacity = new_cap;
        }

        CrashRecord r;
        memset(&r, 0, sizeof(r));
        r.line_no = line_no;
        snprintf(r.date, sizeof(r.date), "%s", fields[COL_DATE]);
        snprintf(r.location, sizeof(r.location), "%s", fields[COL_LOC]);
        snprintf(r.operator_name, sizeof(r.operator_name), "%s", fields[COL_OP]);
        snprintf(r.aircraft, sizeof(r.aircraft), "%s", fields[COL_AC]);
        r.aboard = parse_non_neg_int(fields[COL_ABOARD]);
        r.fatalities = parse_non_neg_int(fields[COL_FATAL]);
        r.ground = (n_cols > COL_GROUND) ? parse_non_neg_int(fields[COL_GROUND]) : -1;
        r.year = extract_year(r.date);

        records[count++] = r;
    }

    fclose(fp);
    *out_count = count;
    return records;
}

/* Feature 1: Audit anomalies & export cleaned_crashes.csv */
void audit_and_clean(const CrashRecord *records, size_t count) {
    FILE *log_fp = fopen("issues_log.txt", "w");
    FILE *clean_fp = fopen("cleaned_crashes.csv", "w");
    if (!log_fp || !clean_fp) {
        perror("Error creating audit/clean output files");
        if (log_fp) fclose(log_fp);
        if (clean_fp) fclose(clean_fp);
        return;
    }

    fprintf(clean_fp, "Date,Year,Location,Operator,Aircraft,Aboard,Fatalities,Ground\n");

    size_t issues = 0;
    for (size_t i = 0; i < count; i++) {
        const CrashRecord *r = &records[i];
        int bad = 0;

        if (r->year < 0) {
            fprintf(log_fp, "Line %d: Invalid/missing year in date '%s'\n", r->line_no, r->date);
            bad = 1;
        }
        if (r->aboard < 0) {
            fprintf(log_fp, "Line %d: Missing/invalid Aboard count\n", r->line_no);
            bad = 1;
        }
        if (r->fatalities < 0) {
            fprintf(log_fp, "Line %d: Missing/invalid Fatalities count\n", r->line_no);
            bad = 1;
        }
        if (r->aboard >= 0 && r->fatalities >= 0 && r->fatalities > r->aboard) {
            fprintf(log_fp, "Line %d: Fatalities (%d) > Aboard (%d)\n",
                    r->line_no, r->fatalities, r->aboard);
            bad = 1;
        }
        if (bad) issues++;

        char loc_esc[400], op_esc[400], ac_esc[400];
        csv_escape(r->location[0] ? r->location : "Unknown", loc_esc, sizeof(loc_esc));
        csv_escape(r->operator_name[0] ? r->operator_name : "Unknown", op_esc, sizeof(op_esc));
        csv_escape(r->aircraft[0] ? r->aircraft : "Unknown", ac_esc, sizeof(ac_esc));

        fprintf(clean_fp, "%s,%d,%s,%s,%s,%d,%d,%d\n",
                r->date[0] ? r->date : "Unknown",
                r->year,
                loc_esc,
                op_esc,
                ac_esc,
                r->aboard,
                r->fatalities,
                r->ground);
    }

    fclose(log_fp);
    fclose(clean_fp);
    printf("Audit complete: %zu flagged rows logged to issues_log.txt\n", issues);
    printf("Cleaned dataset saved to cleaned_crashes.csv\n");
}

/* Feature 2: Decade breakdown & trend_report.txt */
void decade_report(const CrashRecord *records, size_t count) {
    FILE *rp = fopen("trend_report.txt", "w");
    if (!rp) {
        perror("Error creating trend_report.txt");
        return;
    }

    int crashes[13] = {0};
    long long aboard_sum[13] = {0};
    long long fatal_sum[13] = {0};
    long long total_aboard = 0, total_fatal = 0;

    for (size_t i = 0; i < count; i++) {
        const CrashRecord *r = &records[i];
        if (r->aboard >= 0) total_aboard += r->aboard;
        if (r->fatalities >= 0) total_fatal += r->fatalities;

        if (r->year >= 1900 && r->year <= 2029) {
            int idx = (r->year - 1900) / 10;
            crashes[idx]++;
            if (r->aboard >= 0) aboard_sum[idx] += r->aboard;
            if (r->fatalities >= 0) fatal_sum[idx] += r->fatalities;
        }
    }

    double overall_rate = (total_aboard > 0)
        ? ((double)total_fatal / (double)total_aboard) * 100.0
        : 0.0;

    fprintf(rp, "AVIATION CRASH DECADE TREND REPORT\n");
    fprintf(rp, "==================================\n");
    fprintf(rp, "Total Records   : %zu\n", count);
    fprintf(rp, "Total Aboard    : %lld\n", total_aboard);
    fprintf(rp, "Total Fatalities: %lld\n", total_fatal);
    fprintf(rp, "Fatality Rate   : %.2f%%\n\n", overall_rate);
    fprintf(rp, "%-8s %-10s %-12s %-12s %-10s\n",
            "Decade", "Crashes", "Aboard", "Fatalities", "Rate (%)");

    printf("\n%-8s %-10s %-12s %-12s %-10s\n",
           "Decade", "Crashes", "Aboard", "Fatalities", "Rate (%)");
    printf("--------------------------------------------------------\n");

    for (int i = 0; i < 13; i++) {
        if (crashes[i] > 0) {
            int start_yr = 1900 + i * 10;
            double rate = (aboard_sum[i] > 0)
                ? ((double)fatal_sum[i] / (double)aboard_sum[i]) * 100.0
                : 0.0;
            char label[16];
            snprintf(label, sizeof(label), "%ds", start_yr);

            printf("%-8s %-10d %-12lld %-12lld %-10.2f\n",
                   label, crashes[i], aboard_sum[i], fatal_sum[i], rate);
            fprintf(rp, "%-8s %-10d %-12lld %-12lld %-10.2f\n",
                    label, crashes[i], aboard_sum[i], fatal_sum[i], rate);
        }
    }

    fclose(rp);
    printf("Saved report to trend_report.txt\n");
}

static int cmp_summary_desc(const void *a, const void *b) {
    const SummaryItem *x = (const SummaryItem *)a;
    const SummaryItem *y = (const SummaryItem *)b;
    if (y->crashes != x->crashes) return (y->crashes > x->crashes) ? 1 : -1;
    if (y->fatalities != x->fatalities) return (y->fatalities > x->fatalities) ? 1 : -1;
    return strcmp(x->label, y->label);
}

static int cmp_record_fatal_desc(const void *a, const void *b) {
    const CrashRecord *x = (const CrashRecord *)a;
    const CrashRecord *y = (const CrashRecord *)b;
    if (y->fatalities != x->fatalities) return (y->fatalities > x->fatalities) ? 1 : -1;
    return y->aboard - x->aboard;
}

static void add_to_summary(SummaryItem **list, size_t *len, size_t *cap, const char *label, int fat) {
    const char *clean_label = (label && label[0] != '\0') ? label : "Unknown";
    for (size_t i = 0; i < *len; i++) {
        if (strcmp((*list)[i].label, clean_label) == 0) {
            (*list)[i].crashes++;
            if (fat >= 0) (*list)[i].fatalities += fat;
            return;
        }
    }
    if (*len == *cap) {
        size_t new_cap = (*cap == 0) ? 128 : (*cap) * 2;
        SummaryItem *tmp = realloc(*list, new_cap * sizeof(SummaryItem));
        if (!tmp) return;
        *list = tmp;
        *cap = new_cap;
    }
    SummaryItem item;
    snprintf(item.label, sizeof(item.label), "%s", clean_label);
    item.crashes = 1;
    item.fatalities = (fat >= 0) ? fat : 0;
    (*list)[(*len)++] = item;
}

/* Feature 3: Search by Operator, Aircraft, or Year + Top 10 Operators */
void search_and_operators(const CrashRecord *records, size_t count) {
    printf("\n1. Search by Operator keyword\n");
    printf("2. Search by Aircraft keyword\n");
    printf("3. Search by Year\n");
    printf("4. Show Top 10 Operators by Incident Count\n");
    printf("Sub-choice: ");

    int sub = 0;
    if (scanf("%d", &sub) != 1) {
        clear_stdin_line();
        return;
    }
    clear_stdin_line();

    if (sub == 4) {
        SummaryItem *ops = NULL;
        size_t n_ops = 0, cap_ops = 0;
        for (size_t i = 0; i < count; i++) {
            add_to_summary(&ops, &n_ops, &cap_ops, records[i].operator_name, records[i].fatalities);
        }
        qsort(ops, n_ops, sizeof(SummaryItem), cmp_summary_desc);
        printf("\n%-36s %-10s %-12s\n", "Operator", "Crashes", "Fatalities");
        printf("------------------------------------------------------------\n");
        size_t limit = (n_ops < 10) ? n_ops : 10;
        for (size_t i = 0; i < limit; i++) {
            printf("%-36.36s %-10d %-12lld\n", ops[i].label, ops[i].crashes, ops[i].fatalities);
        }
        free(ops);
        return;
    }

    if (sub == 3) {
        int yr = 0;
        printf("Enter year (e.g., 2001): ");
        if (scanf("%d", &yr) != 1) {
            clear_stdin_line();
            return;
        }
        clear_stdin_line();

        int matches = 0, total_yr = 0;
        for (size_t i = 0; i < count; i++) {
            if (records[i].year == yr) {
                total_yr++;
                if (matches < 25) {
                    printf("[%s] %s | %s | Fatalities: %d\n",
                           records[i].date, records[i].operator_name,
                           records[i].aircraft, records[i].fatalities);
                    matches++;
                }
            }
        }
        printf("Total matching records in %d: %d (showing up to 25)\n", yr, total_yr);
        return;
    }

    char query[160] = {0};
    printf("Enter search keyword: ");
    if (!fgets(query, sizeof(query), stdin)) return;
    strip_newline(query);
    trim_spaces(query);

    int shown = 0, total_matches = 0;
    for (size_t i = 0; i < count; i++) {
        int hit = (sub == 1)
            ? contains_case_insensitive(records[i].operator_name, query)
            : contains_case_insensitive(records[i].aircraft, query);
        if (hit) {
            total_matches++;
            if (shown < 25) {
                printf("[%s] %s | %s | %s | Fatalities: %d\n",
                       records[i].date, records[i].operator_name,
                       records[i].aircraft, records[i].location, records[i].fatalities);
                shown++;
            }
        }
    }
    printf("Found %d matching crashes (showing up to 25).\n", total_matches);
}

/* Feature 4: Filter by min fatalities & year range + export CSV */
void filter_and_export(const CrashRecord *records, size_t count) {
    int min_fatal = 0, start_yr = 1900, end_yr = 2026;
    printf("Minimum fatalities (0 for any): ");
    if (scanf("%d", &min_fatal) != 1) { clear_stdin_line(); return; }
    printf("Start year (e.g., 1980): ");
    if (scanf("%d", &start_yr) != 1) { clear_stdin_line(); return; }
    printf("End year (e.g., 2019): ");
    if (scanf("%d", &end_yr) != 1) { clear_stdin_line(); return; }
    clear_stdin_line();

    FILE *out = fopen("filtered_crashes.csv", "w");
    if (!out) {
        perror("Failed to create filtered_crashes.csv");
        return;
    }
    fprintf(out, "Date,Year,Location,Operator,Aircraft,Aboard,Fatalities\n");

    size_t matched = 0;
    for (size_t i = 0; i < count; i++) {
        const CrashRecord *r = &records[i];
        if (r->year < start_yr || r->year > end_yr) continue;
        if (min_fatal > 0 && (r->fatalities < min_fatal)) continue;

        char loc_esc[400], op_esc[400], ac_esc[400];
        csv_escape(r->location, loc_esc, sizeof(loc_esc));
        csv_escape(r->operator_name, op_esc, sizeof(op_esc));
        csv_escape(r->aircraft, ac_esc, sizeof(ac_esc));

        fprintf(out, "%s,%d,%s,%s,%s,%d,%d\n",
                r->date, r->year, loc_esc, op_esc, ac_esc, r->aboard, r->fatalities);
        matched++;
    }

    fclose(out);
    printf("Exported %zu matching records to filtered_crashes.csv\n", matched);
}

/* Feature 5: Top Aircraft & Locations using qsort + top_summary.txt */
void top_aircraft_and_locations(const CrashRecord *records, size_t count) {
    SummaryItem *ac = NULL, *locs = NULL;
    size_t n_ac = 0, cap_ac = 0, n_loc = 0, cap_loc = 0;

    for (size_t i = 0; i < count; i++) {
        add_to_summary(&ac, &n_ac, &cap_ac, records[i].aircraft, records[i].fatalities);
        add_to_summary(&locs, &n_loc, &cap_loc, records[i].location, records[i].fatalities);
    }

    qsort(ac, n_ac, sizeof(SummaryItem), cmp_summary_desc);
    qsort(locs, n_loc, sizeof(SummaryItem), cmp_summary_desc);

    FILE *fp = fopen("top_summary.txt", "w");
    if (fp) fprintf(fp, "TOP 10 AIRCRAFT TYPES\n---------------------\n");
    printf("\nTOP 10 AIRCRAFT TYPES\n");
    for (size_t i = 0; i < n_ac && i < 10; i++) {
        printf("%2zu. %-34.34s | %4d crashes | %7lld fatalities\n",
               i + 1, ac[i].label, ac[i].crashes, ac[i].fatalities);
        if (fp) fprintf(fp, "%2zu. %-34.34s | %4d crashes | %7lld fatalities\n",
                        i + 1, ac[i].label, ac[i].crashes, ac[i].fatalities);
    }

    if (fp) fprintf(fp, "\nTOP 10 LOCATIONS\n----------------\n");
    printf("\nTOP 10 LOCATIONS\n");
    for (size_t i = 0; i < n_loc && i < 10; i++) {
        printf("%2zu. %-34.34s | %4d crashes | %7lld fatalities\n",
               i + 1, locs[i].label, locs[i].crashes, locs[i].fatalities);
        if (fp) fprintf(fp, "%2zu. %-34.34s | %4d crashes | %7lld fatalities\n",
                        i + 1, locs[i].label, locs[i].crashes, locs[i].fatalities);
    }

    if (fp) {
        fclose(fp);
        printf("Saved top rankings to top_summary.txt\n");
    }

    free(ac);
    free(locs);
}

/* Feature 6: Deadliest Crashes & 100% Survival Spotlight */
void deadliest_and_survival_spotlight(const CrashRecord *records, size_t count) {
    CrashRecord *copy = malloc(count * sizeof(CrashRecord));
    if (!copy) {
        perror("Memory allocation failed");
        return;
    }
    memcpy(copy, records, count * sizeof(CrashRecord));
    qsort(copy, count, sizeof(CrashRecord), cmp_record_fatal_desc);

    int zero_fatal_count = 0;
    long long zero_fatal_saved = 0;
    for (size_t i = 0; i < count; i++) {
        if (records[i].fatalities == 0 && records[i].aboard > 0) {
            zero_fatal_count++;
            zero_fatal_saved += records[i].aboard;
        }
    }

    FILE *fp = fopen("spotlight_report.txt", "w");
    printf("\nTOP 10 DEADLIEST RECORDED CRASHES\n");
    printf("---------------------------------\n");
    if (fp) {
        fprintf(fp, "TOP 10 DEADLIEST RECORDED CRASHES\n");
        fprintf(fp, "---------------------------------\n");
    }

    for (size_t i = 0; i < count && i < 10; i++) {
        printf("%2zu. [%s] %-26.26s | %-22.22s | Fatalities: %d / %d\n",
               i + 1, copy[i].date, copy[i].operator_name,
               copy[i].location, copy[i].fatalities, copy[i].aboard);
        if (fp) {
            fprintf(fp, "%2zu. [%s] %s | %s | %s | Fatalities: %d / %d\n",
                    i + 1, copy[i].date, copy[i].operator_name,
                    copy[i].aircraft, copy[i].location,
                    copy[i].fatalities, copy[i].aboard);
        }
    }

    printf("\n100%% SURVIVAL / ZERO-FATALITY INCIDENTS\n");
    printf("---------------------------------------\n");
    printf("Zero-fatality incidents (Aboard > 0) : %d\n", zero_fatal_count);
    printf("Total people aboard who all survived : %lld\n", zero_fatal_saved);

    if (fp) {
        fprintf(fp, "\n100%% SURVIVAL / ZERO-FATALITY INCIDENTS\n");
        fprintf(fp, "---------------------------------------\n");
        fprintf(fp, "Zero-fatality incidents (Aboard > 0) : %d\n", zero_fatal_count);
        fprintf(fp, "Total people aboard who all survived : %lld\n", zero_fatal_saved);
        fclose(fp);
        printf("Saved spotlight report to spotlight_report.txt\n");
    }

    free(copy);
}

/* Feature 7: Military vs. Commercial/Civil Comparison */
void military_vs_commercial(const CrashRecord *records, size_t count) {
    int mil_crashes = 0, civ_crashes = 0;
    long long mil_aboard = 0, civ_aboard = 0;
    long long mil_fatal = 0, civ_fatal = 0;

    for (size_t i = 0; i < count; i++) {
        const CrashRecord *r = &records[i];
        if (is_military_operator(r->operator_name)) {
            mil_crashes++;
            if (r->aboard >= 0) mil_aboard += r->aboard;
            if (r->fatalities >= 0) mil_fatal += r->fatalities;
        } else {
            civ_crashes++;
            if (r->aboard >= 0) civ_aboard += r->aboard;
            if (r->fatalities >= 0) civ_fatal += r->fatalities;
        }
    }

    double mil_rate = (mil_aboard > 0) ? ((double)mil_fatal / (double)mil_aboard) * 100.0 : 0.0;
    double civ_rate = (civ_aboard > 0) ? ((double)civ_fatal / (double)civ_aboard) * 100.0 : 0.0;

    printf("\nSECTOR COMPARISON: MILITARY VS. COMMERCIAL / CIVIL\n");
    printf("-------------------------------------------------------------\n");
    printf("%-22s %-10s %-12s %-12s %-10s\n", "Sector", "Crashes", "Aboard", "Fatalities", "Rate (%)");
    printf("%-22s %-10d %-12lld %-12lld %-10.2f\n",
           "Military", mil_crashes, mil_aboard, mil_fatal, mil_rate);
    printf("%-22s %-10d %-12lld %-12lld %-10.2f\n",
           "Commercial / Civil", civ_crashes, civ_aboard, civ_fatal, civ_rate);

    FILE *fp = fopen("sector_report.txt", "w");
    if (fp) {
        fprintf(fp, "SECTOR COMPARISON: MILITARY VS. COMMERCIAL / CIVIL\n");
        fprintf(fp, "-------------------------------------------------------------\n");
        fprintf(fp, "%-22s %-10s %-12s %-12s %-10s\n", "Sector", "Crashes", "Aboard", "Fatalities", "Rate (%)");
        fprintf(fp, "%-22s %-10d %-12lld %-12lld %-10.2f\n",
                "Military", mil_crashes, mil_aboard, mil_fatal, mil_rate);
        fprintf(fp, "%-22s %-10d %-12lld %-12lld %-10.2f\n",
                "Commercial / Civil", civ_crashes, civ_aboard, civ_fatal, civ_rate);
        fclose(fp);
        printf("Saved sector comparison to sector_report.txt\n");
    }
}

/* Feature 8: Export dashboard_data.json for the Web Frontend */
void export_dashboard_json(const CrashRecord *records, size_t count) {
    FILE *jf = fopen("dashboard_data.json", "w");
    if (!jf) {
        perror("Failed to create dashboard_data.json");
        return;
    }

    long long total_aboard = 0, total_fatal = 0;
    size_t issues = 0;
    int zero_fatal_count = 0;
    int mil_crashes = 0, civ_crashes = 0;
    long long mil_aboard = 0, civ_aboard = 0;
    long long mil_fatal = 0, civ_fatal = 0;
    int dec_crashes[13] = {0};
    long long dec_aboard[13] = {0}, dec_fatal[13] = {0};

    SummaryItem *ops = NULL, *ac = NULL, *locs = NULL;
    size_t n_ops = 0, cap_ops = 0, n_ac = 0, cap_ac = 0, n_loc = 0, cap_loc = 0;

    for (size_t i = 0; i < count; i++) {
        const CrashRecord *r = &records[i];
        if (r->aboard >= 0) total_aboard += r->aboard;
        if (r->fatalities >= 0) total_fatal += r->fatalities;
        if (r->fatalities == 0 && r->aboard > 0) zero_fatal_count++;

        if (r->year < 0 || r->aboard < 0 || r->fatalities < 0 ||
            (r->aboard >= 0 && r->fatalities >= 0 && r->fatalities > r->aboard)) {
            issues++;
        }

        if (is_military_operator(r->operator_name)) {
            mil_crashes++;
            if (r->aboard >= 0) mil_aboard += r->aboard;
            if (r->fatalities >= 0) mil_fatal += r->fatalities;
        } else {
            civ_crashes++;
            if (r->aboard >= 0) civ_aboard += r->aboard;
            if (r->fatalities >= 0) civ_fatal += r->fatalities;
        }

        if (r->year >= 1900 && r->year <= 2029) {
            int idx = (r->year - 1900) / 10;
            dec_crashes[idx]++;
            if (r->aboard >= 0) dec_aboard[idx] += r->aboard;
            if (r->fatalities >= 0) dec_fatal[idx] += r->fatalities;
        }

        add_to_summary(&ops, &n_ops, &cap_ops, r->operator_name, r->fatalities);
        add_to_summary(&ac, &n_ac, &cap_ac, r->aircraft, r->fatalities);
        add_to_summary(&locs, &n_loc, &cap_loc, r->location, r->fatalities);
    }

    qsort(ops, n_ops, sizeof(SummaryItem), cmp_summary_desc);
    qsort(ac, n_ac, sizeof(SummaryItem), cmp_summary_desc);
    qsort(locs, n_loc, sizeof(SummaryItem), cmp_summary_desc);

    CrashRecord *sorted_fatal = NULL;
    if (count > 0) {
        sorted_fatal = malloc(count * sizeof(CrashRecord));
        if (sorted_fatal) {
            memcpy(sorted_fatal, records, count * sizeof(CrashRecord));
            qsort(sorted_fatal, count, sizeof(CrashRecord), cmp_record_fatal_desc);
        }
    }

    double rate = (total_aboard > 0) ? ((double)total_fatal / (double)total_aboard) * 100.0 : 0.0;

    fprintf(jf, "{\n");
    fprintf(jf, "  \"kpis\": {\n");
    fprintf(jf, "    \"total_records\": %zu,\n", count);
    fprintf(jf, "    \"total_aboard\": %lld,\n", total_aboard);
    fprintf(jf, "    \"total_fatalities\": %lld,\n", total_fatal);
    fprintf(jf, "    \"fatality_rate\": %.2f,\n", rate);
    fprintf(jf, "    \"flagged_issues\": %zu,\n", issues);
    fprintf(jf, "    \"zero_fatality_crashes\": %d\n", zero_fatal_count);
    fprintf(jf, "  },\n");

    fprintf(jf, "  \"sectors\": {\n");
    fprintf(jf, "    \"military\": { \"crashes\": %d, \"aboard\": %lld, \"fatalities\": %lld },\n", mil_crashes, mil_aboard, mil_fatal);
    fprintf(jf, "    \"civil\": { \"crashes\": %d, \"aboard\": %lld, \"fatalities\": %lld }\n", civ_crashes, civ_aboard, civ_fatal);
    fprintf(jf, "  },\n");

    fprintf(jf, "  \"decades\": [\n");
    int first_dec = 1;
    for (int i = 0; i < 13; i++) {
        if (dec_crashes[i] > 0) {
            if (!first_dec) fprintf(jf, ",\n");
            first_dec = 0;
            fprintf(jf, "    { \"decade\": \"%ds\", \"crashes\": %d, \"aboard\": %lld, \"fatalities\": %lld }",
                    1900 + i * 10, dec_crashes[i], dec_aboard[i], dec_fatal[i]);
        }
    }
    fprintf(jf, "\n  ],\n");

    char esc[400];
    fprintf(jf, "  \"top_operators\": [\n");
    for (size_t i = 0; i < n_ops && i < 10; i++) {
        json_escape(ops[i].label, esc, sizeof(esc));
        fprintf(jf, "    { \"name\": \"%s\", \"crashes\": %d, \"fatalities\": %lld }%s\n",
                esc, ops[i].crashes, ops[i].fatalities, (i + 1 < n_ops && i + 1 < 10) ? "," : "");
    }
    fprintf(jf, "  ],\n");

    fprintf(jf, "  \"top_aircraft\": [\n");
    for (size_t i = 0; i < n_ac && i < 10; i++) {
        json_escape(ac[i].label, esc, sizeof(esc));
        fprintf(jf, "    { \"name\": \"%s\", \"crashes\": %d, \"fatalities\": %lld }%s\n",
                esc, ac[i].crashes, ac[i].fatalities, (i + 1 < n_ac && i + 1 < 10) ? "," : "");
    }
    fprintf(jf, "  ],\n");

    fprintf(jf, "  \"top_locations\": [\n");
    for (size_t i = 0; i < n_loc && i < 10; i++) {
        json_escape(locs[i].label, esc, sizeof(esc));
        fprintf(jf, "    { \"name\": \"%s\", \"crashes\": %d, \"fatalities\": %lld }%s\n",
                esc, locs[i].crashes, locs[i].fatalities, (i + 1 < n_loc && i + 1 < 10) ? "," : "");
    }
    fprintf(jf, "  ],\n");

    fprintf(jf, "  \"deadliest\": [\n");
    if (sorted_fatal) {
        for (size_t i = 0; i < count && i < 10; i++) {
            char op_e[400], loc_e[400], ac_e[400];
            json_escape(sorted_fatal[i].operator_name, op_e, sizeof(op_e));
            json_escape(sorted_fatal[i].location, loc_e, sizeof(loc_e));
            json_escape(sorted_fatal[i].aircraft, ac_e, sizeof(ac_e));
            fprintf(jf, "    { \"date\": \"%s\", \"operator\": \"%s\", \"aircraft\": \"%s\", \"location\": \"%s\", \"aboard\": %d, \"fatalities\": %d }%s\n",
                    sorted_fatal[i].date, op_e, ac_e, loc_e,
                    sorted_fatal[i].aboard, sorted_fatal[i].fatalities,
                    (i + 1 < count && i + 1 < 10) ? "," : "");
        }
    }
    fprintf(jf, "  ]\n");
    fprintf(jf, "}\n");

    fclose(jf);
    free(ops);
    free(ac);
    free(locs);
    free(sorted_fatal);
    printf("Exported frontend dataset to dashboard_data.json\n");
}

static CrashRecord *build_filtered_subset(const CrashRecord *records, size_t count,
                                          int start_yr, int end_yr, int min_fatal,
                                          size_t *out_sub_count) {
    CrashRecord *sub = malloc((count > 0 ? count : 1) * sizeof(CrashRecord));
    if (!sub) return NULL;
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        const CrashRecord *r = &records[i];
        if (r->year < start_yr || r->year > end_yr) continue;
        if (min_fatal > 0 && r->fatalities < min_fatal) continue;
        sub[n++] = *r;
    }
    *out_sub_count = n;
    return sub;
}

int main(int argc, char **argv) {
    char filename[256] = "Airplane_Crashes_and_Fatalities_Since_1908_20190820105639.csv";

    /* Non-interactive CLI mode for the web frontend:
       ./crash_studio --filter <start_yr> <end_yr> <min_fatal> */
    if (argc == 5 && strcmp(argv[1], "--filter") == 0) {
        int start_yr = atoi(argv[2]);
        int end_yr   = atoi(argv[3]);
        int min_fatal = atoi(argv[4]);

        size_t count = 0;
        CrashRecord *records = load_records(filename, &count);
        if (!records) return 1;

        size_t sub_count = 0;
        CrashRecord *subset = build_filtered_subset(records, count, start_yr, end_yr, min_fatal, &sub_count);
        if (!subset) {
            free(records);
            return 1;
        }

        FILE *out = fopen("filtered_crashes.csv", "w");
        if (out) {
            fprintf(out, "Date,Year,Location,Operator,Aircraft,Aboard,Fatalities\n");
            for (size_t i = 0; i < sub_count; i++) {
                char loc_esc[400], op_esc[400], ac_esc[400];
                csv_escape(subset[i].location, loc_esc, sizeof(loc_esc));
                csv_escape(subset[i].operator_name, op_esc, sizeof(op_esc));
                csv_escape(subset[i].aircraft, ac_esc, sizeof(ac_esc));
                fprintf(out, "%s,%d,%s,%s,%s,%d,%d\n",
                        subset[i].date, subset[i].year, loc_esc, op_esc, ac_esc,
                        subset[i].aboard, subset[i].fatalities);
            }
            fclose(out);
        }

        export_dashboard_json(subset, sub_count);
        printf("Filtered %zu records (%d-%d, min_fatal=%d)\n",
               sub_count, start_yr, end_yr, min_fatal);

        free(subset);
        free(records);
        return 0;
    }

    printf("Enter CSV filename (or press Enter for default): ");
    char input[256];
    if (fgets(input, sizeof(input), stdin)) {
        strip_newline(input);
        trim_spaces(input);
        if (input[0] != '\0') {
            snprintf(filename, sizeof(filename), "%s", input);
        }
    }

    size_t count = 0;
    CrashRecord *records = load_records(filename, &count);
    if (!records) {
        printf("Could not load records from %s\n", filename);
        return 1;
    }
    printf("Loaded %zu crash records.\n", count);

    int choice = -1;
    while (choice != 0) {
        printf("\n=== AVIATION CRASH DATA STUDIO ===\n");
        printf("1. Audit Data & Export Cleaned CSV\n");
        printf("2. Decade Trend Report\n");
        printf("3. Operator / Aircraft / Year Search & Top 10 Operators\n");
        printf("4. Filter & Export Custom CSV\n");
        printf("5. Top Aircraft & Locations\n");
        printf("6. Deadliest Crashes & 100%% Survival Spotlight\n");
        printf("7. Military vs. Commercial/Civil Breakdown\n");
        printf("8. Export JSON for Web Frontend (dashboard_data.json)\n");
        printf("0. Exit\n");
        printf("Choice: ");

        if (scanf("%d", &choice) != 1) {
            clear_stdin_line();
            choice = -1;
            continue;
        }
        clear_stdin_line();

        switch (choice) {
            case 1: audit_and_clean(records, count); break;
            case 2: decade_report(records, count); break;
            case 3: search_and_operators(records, count); break;
            case 4: filter_and_export(records, count); break;
            case 5: top_aircraft_and_locations(records, count); break;
            case 6: deadliest_and_survival_spotlight(records, count); break;
            case 7: military_vs_commercial(records, count); break;
            case 8: export_dashboard_json(records, count); break;
            case 0: printf("Exiting cleanly.\n"); break;
            default: printf("Invalid option.\n"); break;
        }
    }

    free(records);
    return 0;
}

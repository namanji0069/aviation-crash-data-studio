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
    char cause[96];
    int aboard;     /* -1 if missing/invalid */
    int fatalities; /* -1 if missing/invalid */
    int ground;     /* -1 if missing/invalid */
} CrashRecord;

typedef struct {
    char label[160];
    int crashes;
    long long fatalities;
    long long survivors;
} SummaryItem;

typedef struct {
    int year;
    int crashes;
    long long aboard;
    long long fatalities;
    long long survivors;
} AnnualStat;

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

/* Collapse repeated spaces and normalize spacing around hyphens/slashes */
static void normalize_label(const char *src, char *dst, size_t dst_size) {
    if (!src || src[0] == '\0' || dst_size == 0) {
        if (dst_size > 0) snprintf(dst, dst_size, "Unknown");
        return;
    }
    char tmp[MAX_FIELD];
    size_t tpos = 0;
    int prev_space = 0;

    for (size_t i = 0; src[i] != '\0' && tpos + 3 < sizeof(tmp); i++) {
        unsigned char c = (unsigned char)src[i];
        if (isspace(c)) {
            if (!prev_space && tpos > 0) {
                tmp[tpos++] = ' ';
                prev_space = 1;
            }
        } else if (c == '-' || c == '/') {
            while (tpos > 0 && tmp[tpos - 1] == ' ') tpos--;
            tmp[tpos++] = ' ';
            tmp[tpos++] = (char)c;
            tmp[tpos++] = ' ';
            prev_space = 1;
        } else {
            tmp[tpos++] = (char)c;
            prev_space = 0;
        }
    }
    tmp[tpos] = '\0';

    /* Collapse any multiple spaces created around punctuation */
    char clean[MAX_FIELD];
    size_t cpos = 0;
    prev_space = 0;
    for (size_t i = 0; tmp[i] != '\0' && cpos + 1 < sizeof(clean); i++) {
        unsigned char c = (unsigned char)tmp[i];
        if (isspace(c)) {
            if (!prev_space && cpos > 0) {
                clean[cpos++] = ' ';
                prev_space = 1;
            }
        } else {
            clean[cpos++] = (char)c;
            prev_space = 0;
        }
    }
    clean[cpos] = '\0';
    trim_spaces(clean);
    snprintf(dst, dst_size, "%s", clean[0] ? clean : "Unknown");
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
            if (y >= 1900 && y <= 2100) return y;
        }
    }
    return -1;
}

static int contains_case_insensitive(const char *haystack, const char *needle) {
    if (!needle || needle[0] == '\0') return 1;
    if (!haystack || haystack[0] == '\0') return 0;
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
           contains_case_insensitive(op, "indian air force") ||
           contains_case_insensitive(op, "luftwaffe") ||
           contains_case_insensitive(op, "coast guard");
}

static void classify_cause_from_text(const char *text, char *dst, size_t dst_size) {
    const char *label = "Other / Undetermined";
    if (text && text[0] != '\0') {
        if (contains_case_insensitive(text, "shot down") ||
            contains_case_insensitive(text, "missile") ||
            contains_case_insensitive(text, "hijack") ||
            contains_case_insensitive(text, "bomb") ||
            contains_case_insensitive(text, "sabotage")) {
            label = "Hostile Action / Security";
        } else if (contains_case_insensitive(text, "weather") ||
                   contains_case_insensitive(text, "fog") ||
                   contains_case_insensitive(text, "storm") ||
                   contains_case_insensitive(text, "icing") ||
                   contains_case_insensitive(text, "snow") ||
                   contains_case_insensitive(text, "rain")) {
            label = "Weather / Icing / Visibility";
        } else if (contains_case_insensitive(text, "engine") ||
                   contains_case_insensitive(text, "mechanical") ||
                   contains_case_insensitive(text, "fuel") ||
                   contains_case_insensitive(text, "propeller") ||
                   contains_case_insensitive(text, "fire")) {
            label = "Mechanical / Engine / Fire";
        } else if (contains_case_insensitive(text, "mountain") ||
                   contains_case_insensitive(text, "terrain") ||
                   contains_case_insensitive(text, "hill") ||
                   contains_case_insensitive(text, "sea") ||
                   contains_case_insensitive(text, "ocean")) {
            label = "Terrain / Water Impact";
        } else if (contains_case_insensitive(text, "takeoff") ||
                   contains_case_insensitive(text, "landing") ||
                   contains_case_insensitive(text, "approach") ||
                   contains_case_insensitive(text, "runway")) {
            label = "Takeoff / Approach / Landing";
        }
    }
    snprintf(dst, dst_size, "%s", label);
}

static long long calc_survivors(const CrashRecord *r) {
    int ab = (r->aboard >= 0) ? r->aboard : 0;
    int ft = (r->fatalities >= 0) ? r->fatalities : 0;
    return (ab > ft) ? (long long)(ab - ft) : 0LL;
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
        normalize_label(fields[COL_LOC], r.location, sizeof(r.location));
        normalize_label(fields[COL_OP], r.operator_name, sizeof(r.operator_name));
        normalize_label(fields[COL_AC], r.aircraft, sizeof(r.aircraft));
        r.aboard = parse_non_neg_int(fields[COL_ABOARD]);
        r.fatalities = parse_non_neg_int(fields[COL_FATAL]);
        r.ground = (n_cols > COL_GROUND) ? parse_non_neg_int(fields[COL_GROUND]) : -1;
        r.year = extract_year(r.date);

        char combined_clue[MAX_FIELD];
        snprintf(combined_clue, sizeof(combined_clue), "%s %s %s",
                 r.location, r.operator_name, r.aircraft);
        classify_cause_from_text(combined_clue, r.cause, sizeof(r.cause));

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
    long long surv_sum[13] = {0};
    long long total_aboard = 0, total_fatal = 0, total_surv = 0;

    for (size_t i = 0; i < count; i++) {
        const CrashRecord *r = &records[i];
        long long sv = calc_survivors(r);
        if (r->aboard >= 0) total_aboard += r->aboard;
        if (r->fatalities >= 0) total_fatal += r->fatalities;
        total_surv += sv;

        if (r->year >= 1900 && r->year <= 2029) {
            int idx = (r->year - 1900) / 10;
            crashes[idx]++;
            if (r->aboard >= 0) aboard_sum[idx] += r->aboard;
            if (r->fatalities >= 0) fatal_sum[idx] += r->fatalities;
            surv_sum[idx] += sv;
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
    fprintf(rp, "Total Survivors : %lld\n", total_surv);
    fprintf(rp, "Fatality Rate   : %.2f%%\n\n", overall_rate);
    fprintf(rp, "%-8s %-10s %-12s %-12s %-12s %-10s\n",
            "Decade", "Crashes", "Aboard", "Fatalities", "Survivors", "Rate (%)");

    printf("\n%-8s %-10s %-12s %-12s %-12s %-10s\n",
           "Decade", "Crashes", "Aboard", "Fatalities", "Survivors", "Rate (%)");
    printf("--------------------------------------------------------------------\n");

    for (int i = 0; i < 13; i++) {
        if (crashes[i] > 0) {
            int start_yr = 1900 + i * 10;
            double rate = (aboard_sum[i] > 0)
                ? ((double)fatal_sum[i] / (double)aboard_sum[i]) * 100.0
                : 0.0;
            char label[16];
            snprintf(label, sizeof(label), "%ds", start_yr);

            printf("%-8s %-10d %-12lld %-12lld %-12lld %-10.2f\n",
                   label, crashes[i], aboard_sum[i], fatal_sum[i], surv_sum[i], rate);
            fprintf(rp, "%-8s %-10d %-12lld %-12lld %-12lld %-10.2f\n",
                    label, crashes[i], aboard_sum[i], fatal_sum[i], surv_sum[i], rate);
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

static int cmp_annual_asc(const void *a, const void *b) {
    const AnnualStat *x = (const AnnualStat *)a;
    const AnnualStat *y = (const AnnualStat *)b;
    return x->year - y->year;
}

static void add_to_summary(SummaryItem **list, size_t *len, size_t *cap,
                           const char *label, int fat, long long surv) {
    const char *clean_label = (label && label[0] != '\0') ? label : "Unknown";
    for (size_t i = 0; i < *len; i++) {
        if (strcmp((*list)[i].label, clean_label) == 0) {
            (*list)[i].crashes++;
            if (fat >= 0) (*list)[i].fatalities += fat;
            (*list)[i].survivors += surv;
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
    item.survivors = surv;
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
            add_to_summary(&ops, &n_ops, &cap_ops, records[i].operator_name,
                           records[i].fatalities, calc_survivors(&records[i]));
        }
        qsort(ops, n_ops, sizeof(SummaryItem), cmp_summary_desc);
        printf("\n%-36s %-10s %-12s %-12s\n", "Operator", "Crashes", "Fatalities", "Survivors");
        printf("------------------------------------------------------------------------\n");
        size_t limit = (n_ops < 10) ? n_ops : 10;
        for (size_t i = 0; i < limit; i++) {
            printf("%-36.36s %-10d %-12lld %-12lld\n",
                   ops[i].label, ops[i].crashes, ops[i].fatalities, ops[i].survivors);
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
    int min_fatal = 0, start_yr = 1900, end_yr = 2100;
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
        long long sv = calc_survivors(&records[i]);
        add_to_summary(&ac, &n_ac, &cap_ac, records[i].aircraft, records[i].fatalities, sv);
        add_to_summary(&locs, &n_loc, &cap_loc, records[i].location, records[i].fatalities, sv);
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
    CrashRecord *copy = malloc((count > 0 ? count : 1) * sizeof(CrashRecord));
    if (!copy) {
        perror("Memory allocation failed");
        return;
    }
    if (count > 0) {
        memcpy(copy, records, count * sizeof(CrashRecord));
        qsort(copy, count, sizeof(CrashRecord), cmp_record_fatal_desc);
    }

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
    long long mil_surv = 0, civ_surv = 0;

    for (size_t i = 0; i < count; i++) {
        const CrashRecord *r = &records[i];
        long long sv = calc_survivors(r);
        if (is_military_operator(r->operator_name)) {
            mil_crashes++;
            if (r->aboard >= 0) mil_aboard += r->aboard;
            if (r->fatalities >= 0) mil_fatal += r->fatalities;
            mil_surv += sv;
        } else {
            civ_crashes++;
            if (r->aboard >= 0) civ_aboard += r->aboard;
            if (r->fatalities >= 0) civ_fatal += r->fatalities;
            civ_surv += sv;
        }
    }

    double mil_rate = (mil_aboard > 0) ? ((double)mil_fatal / (double)mil_aboard) * 100.0 : 0.0;
    double civ_rate = (civ_aboard > 0) ? ((double)civ_fatal / (double)civ_aboard) * 100.0 : 0.0;

    printf("\nSECTOR COMPARISON: MILITARY VS. COMMERCIAL / CIVIL\n");
    printf("-------------------------------------------------------------------------\n");
    printf("%-22s %-10s %-12s %-12s %-12s %-10s\n",
           "Sector", "Crashes", "Aboard", "Fatalities", "Survivors", "Rate (%)");
    printf("%-22s %-10d %-12lld %-12lld %-12lld %-10.2f\n",
           "Military", mil_crashes, mil_aboard, mil_fatal, mil_surv, mil_rate);
    printf("%-22s %-10d %-12lld %-12lld %-12lld %-10.2f\n",
           "Commercial / Civil", civ_crashes, civ_aboard, civ_fatal, civ_surv, civ_rate);

    FILE *fp = fopen("sector_report.txt", "w");
    if (fp) {
        fprintf(fp, "SECTOR COMPARISON: MILITARY VS. COMMERCIAL / CIVIL\n");
        fprintf(fp, "-------------------------------------------------------------------------\n");
        fprintf(fp, "%-22s %-10s %-12s %-12s %-12s %-10s\n",
                "Sector", "Crashes", "Aboard", "Fatalities", "Survivors", "Rate (%)");
        fprintf(fp, "%-22s %-10d %-12lld %-12lld %-12lld %-10.2f\n",
                "Military", mil_crashes, mil_aboard, mil_fatal, mil_surv, mil_rate);
        fprintf(fp, "%-22s %-10d %-12lld %-12lld %-12lld %-10.2f\n",
                "Commercial / Civil", civ_crashes, civ_aboard, civ_fatal, civ_surv, civ_rate);
        fclose(fp);
        printf("Saved sector comparison to sector_report.txt\n");
    }
}

/* Feature 9: Causal Factor Breakdown & cause_report.txt */
void cause_report(const CrashRecord *records, size_t count) {
    SummaryItem *causes = NULL;
    size_t n_causes = 0, cap_causes = 0;

    for (size_t i = 0; i < count; i++) {
        add_to_summary(&causes, &n_causes, &cap_causes, records[i].cause,
                       records[i].fatalities, calc_survivors(&records[i]));
    }
    qsort(causes, n_causes, sizeof(SummaryItem), cmp_summary_desc);

    FILE *fp = fopen("cause_report.txt", "w");
    printf("\nCAUSAL FACTOR CLASSIFICATION SUMMARY\n");
    printf("-------------------------------------------------------------\n");
    if (fp) {
        fprintf(fp, "CAUSAL FACTOR CLASSIFICATION SUMMARY\n");
        fprintf(fp, "-------------------------------------------------------------\n");
    }

    for (size_t i = 0; i < n_causes; i++) {
        double share = (count > 0) ? ((double)causes[i].crashes / (double)count) * 100.0 : 0.0;
        printf("%-32.32s | %4d crashes (%5.2f%%) | %7lld fatalities\n",
               causes[i].label, causes[i].crashes, share, causes[i].fatalities);
        if (fp) {
            fprintf(fp, "%-32.32s | %4d crashes (%5.2f%%) | %7lld fatalities\n",
                    causes[i].label, causes[i].crashes, share, causes[i].fatalities);
        }
    }

    if (fp) {
        fclose(fp);
        printf("Saved cause breakdown to cause_report.txt\n");
    }
    free(causes);
}

/* Feature 8: Export dashboard_data.json for the Web Frontend */
void export_dashboard_json_ex(
    const CrashRecord *records, size_t count,
    int filter_active, int start_yr, int end_yr, int min_fatal,
    const char *sector, const char *op_q, const char *ac_q, const char *loc_q
) {
    FILE *jf = fopen("dashboard_data.json", "w");
    if (!jf) {
        perror("Failed to create dashboard_data.json");
        return;
    }

    long long total_aboard = 0, total_fatal = 0, total_surv = 0, total_ground = 0;
    size_t issues = 0;
    int zero_fatal_count = 0;
    int mil_crashes = 0, civ_crashes = 0;
    long long mil_aboard = 0, civ_aboard = 0;
    long long mil_fatal = 0, civ_fatal = 0;
    long long mil_surv = 0, civ_surv = 0;
    int dec_crashes[13] = {0};
    long long dec_aboard[13] = {0}, dec_fatal[13] = {0}, dec_surv[13] = {0};

    SummaryItem *ops = NULL, *ac = NULL, *locs = NULL, *causes = NULL;
    size_t n_ops = 0, cap_ops = 0, n_ac = 0, cap_ac = 0, n_loc = 0, cap_loc = 0, n_causes = 0, cap_causes = 0;

    AnnualStat *annual = NULL;
    size_t n_annual = 0, cap_annual = 0;

    for (size_t i = 0; i < count; i++) {
        const CrashRecord *r = &records[i];
        int ab = (r->aboard >= 0) ? r->aboard : 0;
        int ft = (r->fatalities >= 0) ? r->fatalities : 0;
        int gr = (r->ground >= 0) ? r->ground : 0;
        long long sv = calc_survivors(r);

        total_aboard += ab;
        total_fatal += ft;
        total_surv += sv;
        total_ground += gr;
        if (r->fatalities == 0 && r->aboard > 0) zero_fatal_count++;

        if (r->year < 0 || r->aboard < 0 || r->fatalities < 0 ||
            (r->aboard >= 0 && r->fatalities >= 0 && r->fatalities > r->aboard)) {
            issues++;
        }

        if (is_military_operator(r->operator_name)) {
            mil_crashes++;
            mil_aboard += ab;
            mil_fatal += ft;
            mil_surv += sv;
        } else {
            civ_crashes++;
            civ_aboard += ab;
            civ_fatal += ft;
            civ_surv += sv;
        }

        if (r->year >= 1900 && r->year <= 2029) {
            int idx = (r->year - 1900) / 10;
            dec_crashes[idx]++;
            dec_aboard[idx] += ab;
            dec_fatal[idx] += ft;
            dec_surv[idx] += sv;

            int found = 0;
            for (size_t y = 0; y < n_annual; y++) {
                if (annual[y].year == r->year) {
                    annual[y].crashes++;
                    annual[y].aboard += ab;
                    annual[y].fatalities += ft;
                    annual[y].survivors += sv;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                if (n_annual == cap_annual) {
                    size_t nc = (cap_annual == 0) ? 128 : cap_annual * 2;
                    AnnualStat *tmp = realloc(annual, nc * sizeof(AnnualStat));
                    if (tmp) {
                        annual = tmp;
                        cap_annual = nc;
                    }
                }
                if (n_annual < cap_annual) {
                    annual[n_annual].year = r->year;
                    annual[n_annual].crashes = 1;
                    annual[n_annual].aboard = ab;
                    annual[n_annual].fatalities = ft;
                    annual[n_annual].survivors = sv;
                    n_annual++;
                }
            }
        }

        add_to_summary(&ops, &n_ops, &cap_ops, r->operator_name, r->fatalities, sv);
        add_to_summary(&ac, &n_ac, &cap_ac, r->aircraft, r->fatalities, sv);
        add_to_summary(&locs, &n_loc, &cap_loc, r->location, r->fatalities, sv);
        add_to_summary(&causes, &n_causes, &cap_causes, r->cause, r->fatalities, sv);
    }

    qsort(ops, n_ops, sizeof(SummaryItem), cmp_summary_desc);
    qsort(ac, n_ac, sizeof(SummaryItem), cmp_summary_desc);
    qsort(locs, n_loc, sizeof(SummaryItem), cmp_summary_desc);
    qsort(causes, n_causes, sizeof(SummaryItem), cmp_summary_desc);
    if (n_annual > 1) {
        qsort(annual, n_annual, sizeof(AnnualStat), cmp_annual_asc);
    }

    CrashRecord *sorted_fatal = NULL;
    if (count > 0) {
        sorted_fatal = malloc(count * sizeof(CrashRecord));
        if (sorted_fatal) {
            memcpy(sorted_fatal, records, count * sizeof(CrashRecord));
            qsort(sorted_fatal, count, sizeof(CrashRecord), cmp_record_fatal_desc);
        }
    }

    double rate = (total_aboard > 0) ? ((double)total_fatal / (double)total_aboard) * 100.0 : 0.0;
    char esc[400];

    fprintf(jf, "{\n");
    fprintf(jf, "  \"filter\": {\n");
    fprintf(jf, "    \"active\": %s,\n", filter_active ? "true" : "false");
    fprintf(jf, "    \"start_year\": %d,\n", start_yr);
    fprintf(jf, "    \"end_year\": %d,\n", end_yr);
    fprintf(jf, "    \"min_fatalities\": %d,\n", min_fatal);
    json_escape(sector ? sector : "all", esc, sizeof(esc));
    fprintf(jf, "    \"sector\": \"%s\",\n", esc);
    json_escape(op_q ? op_q : "", esc, sizeof(esc));
    fprintf(jf, "    \"operator\": \"%s\",\n", esc);
    json_escape(ac_q ? ac_q : "", esc, sizeof(esc));
    fprintf(jf, "    \"aircraft\": \"%s\",\n", esc);
    json_escape(loc_q ? loc_q : "", esc, sizeof(esc));
    fprintf(jf, "    \"location\": \"%s\"\n", esc);
    fprintf(jf, "  },\n");

    fprintf(jf, "  \"kpis\": {\n");
    fprintf(jf, "    \"total_records\": %zu,\n", count);
    fprintf(jf, "    \"total_aboard\": %lld,\n", total_aboard);
    fprintf(jf, "    \"total_fatalities\": %lld,\n", total_fatal);
    fprintf(jf, "    \"total_survivors\": %lld,\n", total_surv);
    fprintf(jf, "    \"total_ground\": %lld,\n", total_ground);
    fprintf(jf, "    \"fatality_rate\": %.2f,\n", rate);
    fprintf(jf, "    \"flagged_issues\": %zu,\n", issues);
    fprintf(jf, "    \"zero_fatality_crashes\": %d\n", zero_fatal_count);
    fprintf(jf, "  },\n");

    fprintf(jf, "  \"sectors\": {\n");
    fprintf(jf, "    \"military\": { \"crashes\": %d, \"aboard\": %lld, \"fatalities\": %lld, \"survivors\": %lld },\n",
            mil_crashes, mil_aboard, mil_fatal, mil_surv);
    fprintf(jf, "    \"civil\": { \"crashes\": %d, \"aboard\": %lld, \"fatalities\": %lld, \"survivors\": %lld }\n",
            civ_crashes, civ_aboard, civ_fatal, civ_surv);
    fprintf(jf, "  },\n");

    fprintf(jf, "  \"decades\": [\n");
    int first_dec = 1;
    for (int i = 0; i < 13; i++) {
        if (dec_crashes[i] > 0) {
            if (!first_dec) fprintf(jf, ",\n");
            first_dec = 0;
            fprintf(jf, "    { \"decade\": \"%ds\", \"crashes\": %d, \"aboard\": %lld, \"fatalities\": %lld, \"survivors\": %lld }",
                    1900 + i * 10, dec_crashes[i], dec_aboard[i], dec_fatal[i], dec_surv[i]);
        }
    }
    fprintf(jf, "\n  ],\n");

    fprintf(jf, "  \"annual\": [\n");
    for (size_t i = 0; i < n_annual; i++) {
        fprintf(jf, "    { \"year\": %d, \"crashes\": %d, \"aboard\": %lld, \"fatalities\": %lld, \"survivors\": %lld }%s\n",
                annual[i].year, annual[i].crashes, annual[i].aboard, annual[i].fatalities, annual[i].survivors,
                (i + 1 < n_annual) ? "," : "");
    }
    fprintf(jf, "  ],\n");

    fprintf(jf, "  \"causes\": [\n");
    for (size_t i = 0; i < n_causes; i++) {
        json_escape(causes[i].label, esc, sizeof(esc));
        fprintf(jf, "    { \"cause\": \"%s\", \"crashes\": %d, \"fatalities\": %lld, \"survivors\": %lld }%s\n",
                esc, causes[i].crashes, causes[i].fatalities, causes[i].survivors,
                (i + 1 < n_causes) ? "," : "");
    }
    fprintf(jf, "  ],\n");

    fprintf(jf, "  \"top_operators\": [\n");
    for (size_t i = 0; i < n_ops && i < 10; i++) {
        json_escape(ops[i].label, esc, sizeof(esc));
        fprintf(jf, "    { \"name\": \"%s\", \"crashes\": %d, \"fatalities\": %lld, \"survivors\": %lld }%s\n",
                esc, ops[i].crashes, ops[i].fatalities, ops[i].survivors, (i + 1 < n_ops && i + 1 < 10) ? "," : "");
    }
    fprintf(jf, "  ],\n");

    fprintf(jf, "  \"top_aircraft\": [\n");
    for (size_t i = 0; i < n_ac && i < 10; i++) {
        json_escape(ac[i].label, esc, sizeof(esc));
        fprintf(jf, "    { \"name\": \"%s\", \"crashes\": %d, \"fatalities\": %lld, \"survivors\": %lld }%s\n",
                esc, ac[i].crashes, ac[i].fatalities, ac[i].survivors, (i + 1 < n_ac && i + 1 < 10) ? "," : "");
    }
    fprintf(jf, "  ],\n");

    fprintf(jf, "  \"top_locations\": [\n");
    for (size_t i = 0; i < n_loc && i < 10; i++) {
        json_escape(locs[i].label, esc, sizeof(esc));
        fprintf(jf, "    { \"name\": \"%s\", \"crashes\": %d, \"fatalities\": %lld, \"survivors\": %lld }%s\n",
                esc, locs[i].crashes, locs[i].fatalities, locs[i].survivors, (i + 1 < n_loc && i + 1 < 10) ? "," : "");
    }
    fprintf(jf, "  ],\n");

    fprintf(jf, "  \"deadliest\": [\n");
    if (sorted_fatal) {
        for (size_t i = 0; i < count && i < 10; i++) {
            char op_e[400], loc_e[400], ac_e[400];
            json_escape(sorted_fatal[i].operator_name, op_e, sizeof(op_e));
            json_escape(sorted_fatal[i].location, loc_e, sizeof(loc_e));
            json_escape(sorted_fatal[i].aircraft, ac_e, sizeof(ac_e));
            fprintf(jf, "    { \"date\": \"%s\", \"operator\": \"%s\", \"aircraft\": \"%s\", \"location\": \"%s\", \"aboard\": %d, \"fatalities\": %d, \"survivors\": %lld }%s\n",
                    sorted_fatal[i].date, op_e, ac_e, loc_e,
                    sorted_fatal[i].aboard, sorted_fatal[i].fatalities, calc_survivors(&sorted_fatal[i]),
                    (i + 1 < count && i + 1 < 10) ? "," : "");
        }
    }
    fprintf(jf, "  ]\n");
    fprintf(jf, "}\n");

    fclose(jf);
    free(ops);
    free(ac);
    free(locs);
    free(causes);
    free(annual);
    free(sorted_fatal);
    printf("Exported frontend dataset to dashboard_data.json\n");
}

void export_dashboard_json(const CrashRecord *records, size_t count) {
    export_dashboard_json_ex(records, count, 0, 1908, 2019, 0, "all", "", "", "");
}

static CrashRecord *build_filtered_subset_ex(
    const CrashRecord *records, size_t count,
    int start_yr, int end_yr, int min_fatal,
    const char *sector, const char *op_q, const char *ac_q, const char *loc_q,
    size_t *out_sub_count
) {
    CrashRecord *sub = malloc((count > 0 ? count : 1) * sizeof(CrashRecord));
    if (!sub) return NULL;
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        const CrashRecord *r = &records[i];
        if (r->year < start_yr || r->year > end_yr) continue;
        if (min_fatal > 0 && r->fatalities < min_fatal) continue;
        if (sector && strcmp(sector, "military") == 0 && !is_military_operator(r->operator_name)) continue;
        if (sector && strcmp(sector, "civil") == 0 && is_military_operator(r->operator_name)) continue;
        if (op_q && op_q[0] != '\0' && !contains_case_insensitive(r->operator_name, op_q)) continue;
        if (ac_q && ac_q[0] != '\0' && !contains_case_insensitive(r->aircraft, ac_q)) continue;
        if (loc_q && loc_q[0] != '\0' && !contains_case_insensitive(r->location, loc_q)) continue;
        sub[n++] = *r;
    }
    *out_sub_count = n;
    return sub;
}

int main(int argc, char **argv) {
    char filename[256] = "Airplane_Crashes_and_Fatalities_Since_1908_20190820105639.csv";

    /* Non-interactive CLI mode for the web frontend:
       ./crash_studio --filter <start_yr> <end_yr> <min_fatal> [sector] [operator] [aircraft] [location] */
    if (argc >= 5 && strcmp(argv[1], "--filter") == 0) {
        int start_yr = atoi(argv[2]);
        int end_yr   = atoi(argv[3]);
        int min_fatal = atoi(argv[4]);
        const char *sector = (argc >= 6 && argv[5][0]) ? argv[5] : "all";
        const char *op_q   = (argc >= 7) ? argv[6] : "";
        const char *ac_q   = (argc >= 8) ? argv[7] : "";
        const char *loc_q  = (argc >= 9) ? argv[8] : "";

        size_t count = 0;
        CrashRecord *records = load_records(filename, &count);
        if (!records) return 1;

        size_t sub_count = 0;
        CrashRecord *subset = build_filtered_subset_ex(
            records, count, start_yr, end_yr, min_fatal,
            sector, op_q, ac_q, loc_q, &sub_count
        );
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

        export_dashboard_json_ex(subset, sub_count, 1, start_yr, end_yr, min_fatal, sector, op_q, ac_q, loc_q);
        cause_report(subset, sub_count);
        printf("Filtered %zu records (%d-%d, min_fatal=%d, sector=%s)\n",
               sub_count, start_yr, end_yr, min_fatal, sector);

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
        printf("9. Causal Factor Breakdown (cause_report.txt)\n");
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
            case 9: cause_report(records, count); break;
            case 0: printf("Exiting cleanly.\n"); break;
            default: printf("Invalid option.\n"); break;
        }
    }

    free(records);
    return 0;
}

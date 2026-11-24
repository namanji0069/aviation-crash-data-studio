# Aviation Crash Data Studio

An aviation crash analytics system combining a C11 data-processing and reporting backend (main.c), a Python HTTP bridge (serve.py), and an interactive web dashboard (index.html).

## Features
1. Data auditing & anomaly logging (issues_log.txt, cleaned_crashes.csv)
2. Decade trend analysis (trend_report.txt)
3. Operator, aircraft, and year search + Top 10 operators
4. Custom timeline & fatality filtering + CSV export (filtered_crashes.csv and --filter CLI mode)
5. Top 10 aircraft types & locations (top_summary.txt)
6. Deadliest crashes & 100% survival spotlight (spotlight_report.txt)
7. Military vs. Commercial / Civil comparison (sector_report.txt)
8. Dashboard JSON export (dashboard_data.json)

## Build & Run
clang -Wall -Wextra -std=c11 main.c -o crash_studio
./crash_studio
python3 serve.py

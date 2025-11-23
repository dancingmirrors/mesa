#!/bin/bash
# Whitespace checker for C files, headers, and Python files in src/intel
# This script checks for accidental whitespace issues:
# 1. Lines with only whitespace (should be completely empty)
# 2. Lines ending with whitespace
# 3. Lines with = followed by space at end (unnecessary space before continuation)

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

total_issues=0
files_with_issues=0

check_file() {
    local file="$1"
    local file_issues=0
    local output=""

    # Check for lines with only whitespace
    local whitespace_only=$(grep -n "^[[:space:]]\+$" "$file" 2>/dev/null | wc -l)
    if [ $whitespace_only -gt 0 ]; then
        output+="  ${YELLOW}Lines with only whitespace ($whitespace_only):${NC}\n"
        while IFS= read -r line; do
            output+="    Line $(echo "$line" | cut -d: -f1)\n"
        done < <(grep -n "^[[:space:]]\+$" "$file" | head -10)
        file_issues=$((file_issues + whitespace_only))
    fi

    # Check for lines ending with whitespace (excluding empty lines)
    local trailing_whitespace=$(grep -n "[^[:space:]][[:space:]]\+$" "$file" 2>/dev/null | wc -l)
    if [ $trailing_whitespace -gt 0 ]; then
        output+="  ${YELLOW}Lines ending with whitespace ($trailing_whitespace):${NC}\n"
        while IFS= read -r line; do
            line_num=$(echo "$line" | cut -d: -f1)
            output+="    Line $line_num\n"
        done < <(grep -n "[^[:space:]][[:space:]]\+$" "$file" | head -10)
        file_issues=$((file_issues + trailing_whitespace))
    fi

    # Check for lines ending with = and space (continuation lines with unnecessary space)
    local equals_space=$(grep -n "=[[:space:]]\+$" "$file" 2>/dev/null | wc -l)
    if [ $equals_space -gt 0 ]; then
        output+="  ${YELLOW}Lines ending with '= ' ($equals_space):${NC}\n"
        while IFS= read -r line; do
            line_num=$(echo "$line" | cut -d: -f1)
            output+="    Line $line_num\n"
        done < <(grep -n "=[[:space:]]\+$" "$file" | head -10)
        file_issues=$((file_issues + equals_space))
    fi

    if [ $file_issues -gt 0 ]; then
        echo -e "${RED}File: $file${NC}"
        echo -e "$output"
        total_issues=$((total_issues + file_issues))
        files_with_issues=$((files_with_issues + 1))
    fi
}

echo "=== Checking .c files in src/intel ==="
c_file_count=0
while IFS= read -r file; do
    check_file "$file"
    c_file_count=$((c_file_count + 1))
done < <(find src/intel -name "*.c" -type f | grep -v ".git" | grep -v "^\./build/" | grep -v "^\./build-32/")

echo ""
echo "=== Checking .h header files in src/intel ==="
h_file_count=0
while IFS= read -r file; do
    check_file "$file"
    h_file_count=$((h_file_count + 1))
done < <(find src/intel -name "*.h" -type f | grep -v ".git" | grep -v "^\./build/" | grep -v "^\./build-32/")

echo ""
echo "=== Checking .py files in src/intel ==="
py_file_count=0
while IFS= read -r file; do
    check_file "$file"
    py_file_count=$((py_file_count + 1))
done < <(find src/intel -name "*.py" -type f | grep -v ".git" | grep -v "^\./build/" | grep -v "^\./build-32/")

echo ""
echo "=== Summary ==="
echo "C files checked: $c_file_count"
echo "Header files checked: $h_file_count"
echo "Python files checked: $py_file_count"
echo "Files with issues: $files_with_issues"
echo "Total issues found: $total_issues"

if [ $total_issues -eq 0 ]; then
    echo -e "${GREEN}✓ No whitespace issues found!${NC}"
    exit 0
else
    echo -e "${RED}✗ Found $total_issues whitespace issues in $files_with_issues files${NC}"
    exit 1
fi

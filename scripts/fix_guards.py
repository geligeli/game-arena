#!/usr/bin/env python3
import sys
import os
import re
import subprocess

def get_git_root():
    try:
        root = subprocess.check_output(['git', 'rev-parse', '--show-toplevel'])
        return root.decode('utf-8').strip()
    except subprocess.CalledProcessError:
        return None

def generate_guard_name(git_root, file_path):
    # Get the repository folder name
    repo_name = os.path.basename(git_root).upper()
    
    # Get path relative to git root
    rel_path = os.path.relpath(file_path, git_root)
    
    # Sanitize: Replace non-alphanumeric characters with _ and uppercase
    sanitized_path = re.sub(r'[^a-zA-Z0-9]', '_', rel_path).upper()
    repo_name = re.sub(r'[^a-zA-Z0-9]', '_', repo_name).upper()
    
    # Result: REPO_PATH_TO_FILE_H
    return f"{repo_name}_{sanitized_path}"

def process_file(file_path, git_root):
    with open(file_path, 'r', encoding='utf-8') as f:
        content = f.read()

    # Regex to find #pragma once (handling optional whitespace)
    pragma_regex = r'^\s*#pragma\s+once\s*$'
    
    if not re.search(pragma_regex, content, re.MULTILINE):
        return False # No pragma once found, skip

    guard_name = generate_guard_name(git_root, file_path)
    
    # 1. Replace #pragma once with ifndef/define
    header_guard_start = f"#ifndef {guard_name}\n#define {guard_name}"
    new_content = re.sub(pragma_regex, header_guard_start, content, count=1, flags=re.MULTILINE)

    # 2. Append #endif at the very end
    # Ensure content ends with a newline before appending if needed
    if not new_content.endswith('\n'):
        new_content += '\n'
    
    new_content += f"\n#endif // {guard_name}\n"

    with open(file_path, 'w', encoding='utf-8') as f:
        f.write(new_content)
    
    return True

def main():
    git_root = get_git_root()
    if not git_root:
        print("Error: Not inside a git repository.")
        sys.exit(1)

    # Arguments passed from git hook (file paths)
    files = sys.argv[1:]
    files_changed = 0

    for file_path in files:
        if not os.path.exists(file_path): 
            continue
            
        # Only process headers/inl files
        if not file_path.endswith(('.h', '.hpp', '.inl')):
            continue

        try:
            changed = process_file(file_path, git_root)
            if changed:
                print(f"Fixed include guard: {file_path}")
                files_changed += 1
        except Exception as e:
            print(f"Error processing {file_path}: {e}")

    # Exit with code 1 if files were changed to stop the commit
    if files_changed > 0:
        print(f"\n{files_changed} files were modified. Please stage them and commit again.")
        sys.exit(1)
    
    sys.exit(0)

if __name__ == "__main__":
    main()
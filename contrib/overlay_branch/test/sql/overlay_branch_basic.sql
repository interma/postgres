-- contrib/overlay_branch/sql/overlay_branch_basic.sql
-- Skeleton smoke test: install the extension, call all SQL functions.

CREATE EXTENSION overlay_branch;

-- 1. Create a branch (stub)
SELECT create_branch('b1');

-- 2. Enter the branch (stub)
SELECT use_branch('b1');

-- 3. Show current branch
SELECT current_branch();

-- 4. List all branches (skeleton returns empty set for now)
SELECT * FROM list_branches();

-- 5. Try applying (stub)
SELECT apply_branch('b1');

-- 6. Create + discard (stub)
SELECT create_branch('b_discard');
SELECT discard_branch('b_discard');

-- 7. GUC-style switch
SET overlay_branch.current = 'b_guc_test';
SELECT current_branch();

RESET overlay_branch.current;
SELECT current_branch();

-- 8. Clean up (best-effort; skeleton stubs are idempotent)
DROP EXTENSION overlay_branch CASCADE;

# Debug Session: zblog-post-blank
- **Status**: [OPEN]
- **Issue**: `http://127.0.0.1:19044/post/1.html` returns HTTP 200 but renders a blank page.
- **Debug Server**: http://127.0.0.1:7777/event
- **Log File**: `.dbg/trae-debug-log-zblog-post-blank.ndjson`

## Reproduction Steps
1. Start the zblog Nginx and OpenASP FPM services.
2. Request `http://127.0.0.1:19044/post/1.html`.
3. Observe an HTTP 200 response with no visible page content.

## Hypotheses & Verification
| ID | Hypothesis | Likelihood | Effort | Evidence |
|----|------------|------------|--------|----------|
| A | A runtime error after the database query is swallowed by `On Error Resume Next`. | High | Low | Rejected: URL lookup completed normally. |
| B | Template loading succeeds but variable replacement or property access terminates rendering. | High | Medium | Rejected: article loading never succeeds. |
| C | MySQL Recordset field types or cursor behavior differ from Z-Blog expectations. | Medium | Medium | Rejected: empty BOF/EOF matches the database state. |
| D | Response charset conversion fails after output has been generated. | Medium | Low | Rejected: server returns an intentional 404 with a 39-byte whitespace body. |
| E | The rewritten article ID is not passed to FastCGI correctly. | Low | Low | Rejected: `id=1` and `QUERY_STRING=id=1` are present. |

## Log Evidence
1. `pre-fix` line 1: Nginx rewrite passes `id=1` and `QUERY_STRING=id=1`.
2. `pre-fix` line 2: article URL lookup returns `BOF=True`, `EOF=True`.
3. `pre-fix` line 3: `Article.LoadInfoByID(1)` reaches the missing-record branch.
4. MySQL contains only page `ID=2`; article `ID=1` was deleted.
5. Access logs show article IDs 4, 3, and 1 deleted at 17:40:14, 17:40:17, and 17:40:19.
6. Homepage cache timestamp is 17:40:17 and still references ID 1. Deleting the final article did not overwrite it because `TArticleList.Export` returns False for an empty default list.

## Verification Conclusion
Root cause confirmed: deleting the final normal article leaves the previous homepage cache intact, and the missing-article path returns a whitespace-only 404 body.

Pre-fix:
- Homepage cache contains one `/post/1.html` link to a deleted record.
- `/post/1.html` returns HTTP 404 with a 39-byte whitespace-only body.

Post-fix:
- Empty default article lists export a valid homepage and overwrite stale cache.
- Homepage contains zero stale `/post/1.html` links and returns HTTP 200 with 3755 bytes.
- `/post/1.html` returns HTTP 404 with a 1010-byte error page containing `该文章不存在`.
- Guestbook, direct page ID 2, tags, login, and theme CSS all return HTTP 200.

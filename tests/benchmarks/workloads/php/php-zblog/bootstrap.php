<?php

declare(strict_types=1);

const BLOG_NAME = 'OpenASP Benchmark Blog';
const POSTS_PER_PAGE = 10;

function blog_db(): PDO
{
    static $database = null;
    if ($database instanceof PDO) {
        return $database;
    }

    $path = getenv('PHP_ZBLOG_DB');
    if ($path === false || $path === '') {
        throw new RuntimeException('PHP_ZBLOG_DB is not configured');
    }

    $database = new PDO('sqlite:' . $path, null, null, [
        PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
        PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
        PDO::ATTR_PERSISTENT => true,
        PDO::ATTR_STRINGIFY_FETCHES => false,
    ]);
    $database->exec('PRAGMA query_only = ON');
    return $database;
}

function blog_session(): array
{
    if (session_status() !== PHP_SESSION_ACTIVE) {
        session_name('PHPBLOGSID');
        session_start(['read_and_close' => true, 'use_strict_mode' => true]);
    }
    return $_SESSION;
}

function blog_escape(string $value): string
{
    return htmlspecialchars($value, ENT_QUOTES | ENT_SUBSTITUTE, 'UTF-8');
}

function blog_tags(PDO $database): array
{
    $statement = $database->query(
        'SELECT t.name, COUNT(pt.post_id) AS post_count
         FROM tags t
         JOIN post_tags pt ON pt.tag_id = t.id
         GROUP BY t.id, t.name
         ORDER BY post_count DESC, t.name'
    );
    return $statement->fetchAll();
}

function blog_archives(PDO $database): array
{
    $statement = $database->query(
        "SELECT substr(published_at, 1, 7) AS month, COUNT(*) AS post_count
         FROM posts
         GROUP BY month
         ORDER BY month DESC
         LIMIT 12"
    );
    return $statement->fetchAll();
}

function blog_post_tags(PDO $database, int $postId): array
{
    $statement = $database->prepare(
        'SELECT t.name
         FROM tags t
         JOIN post_tags pt ON pt.tag_id = t.id
         WHERE pt.post_id = :post_id
         ORDER BY t.name'
    );
    $statement->execute(['post_id' => $postId]);
    return $statement->fetchAll(PDO::FETCH_COLUMN);
}

function blog_render_post(PDO $database, array $post, bool $full = false): string
{
    $tags = array_map(
        static fn(string $tag): string => '<a href="/tags.php?tag=' . rawurlencode($tag) . '">' . blog_escape($tag) . '</a>',
        blog_post_tags($database, (int)$post['id'])
    );
    $text = $full ? (string)$post['body'] : (string)$post['summary'];
    $paragraphs = array_filter(explode("\n\n", $text), static fn(string $part): bool => $part !== '');
    $body = implode('', array_map(static fn(string $part): string => '<p>' . blog_escape($part) . '</p>', $paragraphs));

    return '<article><header><h2><a href="/view.php?id=' . (int)$post['id'] . '">' .
        blog_escape((string)$post['title']) . '</a></h2><time>' .
        blog_escape((string)$post['published_at']) . '</time></header>' . $body .
        '<footer><span>' . implode(', ', $tags) . '</span><span>' . (int)$post['views'] .
        ' views</span></footer></article>';
}

function blog_render_page(string $title, string $content, PDO $database): void
{
    $session = blog_session();
    $tags = blog_tags($database);
    $archives = blog_archives($database);
    $visitor = isset($session['visitor']) ? (string)$session['visitor'] : 'guest';

    header('Content-Type: text/html; charset=utf-8');
    echo '<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">';
    echo '<title>', blog_escape($title), ' - ', BLOG_NAME, '</title>';
    echo '<style>body{font:15px system-ui;margin:0;color:#242424;background:#f4f5f7}header,main,footer{max-width:1060px;margin:auto}' .
        'body>header{padding:24px 20px;background:#fff;border-bottom:1px solid #ddd}nav a{margin-right:18px;color:#185ea8}' .
        'main{display:grid;grid-template-columns:minmax(0,1fr) 260px;gap:24px;padding:24px 20px}article{background:#fff;padding:18px;margin-bottom:16px}' .
        'article h2{margin:0 0 8px;font-size:21px}article footer{display:flex;justify-content:space-between;color:#666}' .
        'aside{background:#fff;padding:18px;align-self:start}aside ul{padding-left:18px}a{color:#185ea8;text-decoration:none}' .
        'body>footer{padding:20px;color:#666}@media(max-width:720px){main{grid-template-columns:1fr}}</style></head><body>';
    echo '<header><h1>', BLOG_NAME, '</h1><nav><a href="/default.php">Home</a><a href="/search.php">Search</a>';
    echo '<a href="/tags.php">Tags</a><a href="/feed.php">Feed</a></nav></header><main><section>', $content, '</section><aside>';
    echo '<h3>Popular tags</h3><ul>';
    foreach ($tags as $tag) {
        echo '<li>', blog_escape((string)$tag['name']), ' (', (int)$tag['post_count'], ')</li>';
    }
    echo '</ul><h3>Archives</h3><ul>';
    foreach ($archives as $archive) {
        echo '<li>', blog_escape((string)$archive['month']), ' (', (int)$archive['post_count'], ')</li>';
    }
    echo '</ul></aside></main><footer>Visitor: ', blog_escape($visitor), ' | Powered by PHP-FPM</footer></body></html>';
}

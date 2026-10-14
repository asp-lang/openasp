<?php

declare(strict_types=1);

const BLOG_NAME = 'OpenASP Benchmark Blog';
const POSTS_PER_PAGE = 10;

function blog_redis(): Redis
{
    $host = getenv('ZBLOG_REDIS_HOST') ?: '127.0.0.1';
    $port = (int)(getenv('ZBLOG_REDIS_PORT') ?: 16379);
    $redis = new Redis();
    if (!$redis->pconnect($host, $port, 2.0, "zblog-{$host}-{$port}", 0, 2.0)) {
        throw new RuntimeException("Redis connection failed: {$host}:{$port}");
    }
    $redis->setOption(Redis::OPT_SERIALIZER, Redis::SERIALIZER_NONE);
    return $redis;
}

function blog_memcache(): Memcache
{
    $host = getenv('ZBLOG_MEMCACHE_HOST') ?: '127.0.0.1';
    $port = (int)(getenv('ZBLOG_MEMCACHE_PORT') ?: 19146);
    $cache = new Memcache();
    if (!$cache->pconnect($host, $port, 2.0)) {
        throw new RuntimeException("Memcache connection failed: {$host}:{$port}");
    }
    return $cache;
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

/** @return list<array{id:string,title:string,slug:string,summary:string,body:string,published_at:string,views:string}> */
function blog_load_posts(Redis $redis, string $indexKey, int $first, int $last): array
{
    $ids = $redis->lRange($indexKey, $first, $last);
    if (!is_array($ids) || $ids === []) {
        return [];
    }
    $keys = array_map(static fn(string $id): string => "zblog:post:{$id}", $ids);
    $encoded = $redis->mGet($keys);
    return array_map('blog_decode_post', $encoded);
}

/** @return array{id:string,title:string,slug:string,summary:string,body:string,published_at:string,views:string} */
function blog_decode_post(string $encoded): array
{
    [$id, $title, $slug, $summary, $body, $publishedAt, $views] = explode("\t", $encoded);
    return compact('id', 'title', 'slug', 'summary', 'body', 'publishedAt', 'views') + [
        'published_at' => $publishedAt,
    ];
}

/** @return list<array{name:string,post_count:int}> */
function blog_metadata(Memcache $cache, string $key): array
{
    $encoded = $cache->get($key);
    if ($encoded === false || $encoded === '') {
        return [];
    }
    return array_map(static function (string $entry): array {
        [$name, $count] = explode("\t", $entry);
        return ['name' => $name, 'post_count' => (int)$count];
    }, explode("\n", $encoded));
}

/** @param list<array{id:string}> $posts @return array<string, string> */
function blog_load_post_tags(Memcache $cache, array $posts): array
{
    if ($posts === []) {
        return [];
    }
    $keys = array_map(static fn(array $post): string => "zblog:posttags:{$post['id']}", $posts);
    $values = $cache->get($keys);
    return is_array($values) ? $values : [];
}

/** @param array<string, string> $tagValues @return list<string> */
function blog_post_tags(array $tagValues, int $postId): array
{
    $encoded = $tagValues["zblog:posttags:{$postId}"] ?? false;
    return $encoded === false || $encoded === '' ? [] : explode(',', $encoded);
}

/** @param array<string, string> $tagValues */
function blog_render_post(array $tagValues, array $post, bool $full = false): string
{
    $tags = array_map(
        static fn(string $tag): string => '<a href="/tags.php?tag=' . rawurlencode($tag) . '">' . blog_escape($tag) . '</a>',
        blog_post_tags($tagValues, (int)$post['id'])
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

function blog_render_page(string $title, string $content, Memcache $cache): void
{
    $session = blog_session();
    $tags = blog_metadata($cache, 'zblog:tags');
    $archives = blog_metadata($cache, 'zblog:archives');
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
        echo '<li>', blog_escape($tag['name']), ' (', $tag['post_count'], ')</li>';
    }
    echo '</ul><h3>Archives</h3><ul>';
    foreach ($archives as $archive) {
        echo '<li>', blog_escape($archive['name']), ' (', $archive['post_count'], ')</li>';
    }
    echo '</ul></aside></main><footer>Visitor: ', blog_escape($visitor), ' | Powered by PHP-FPM</footer></body></html>';
}

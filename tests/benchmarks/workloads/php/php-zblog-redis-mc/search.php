<?php

declare(strict_types=1);

require __DIR__ . '/bootstrap.php';

$redis = blog_redis();
$cache = blog_memcache();
$query = strtolower(trim((string)($_GET['q'] ?? 'runtime')));
$posts = blog_load_posts($redis, "zblog:search:{$query}", 0, 19);
$postTags = blog_load_post_tags($cache, $posts);
$content = '<h2>Search: ' . blog_escape($query) . '</h2><p>' . count($posts) . ' matching articles.</p>';
foreach ($posts as $post) {
    $content .= blog_render_post($postTags, $post);
}
blog_render_page('Search', $content, $cache);

<?php

declare(strict_types=1);

require __DIR__ . '/bootstrap.php';

$redis = blog_redis();
$cache = blog_memcache();
$postId = max(1, filter_input(INPUT_GET, 'id', FILTER_VALIDATE_INT) ?: 1);
$encoded = $redis->get("zblog:post:{$postId}");
if ($encoded === null) {
    http_response_code(404);
    $content = '<h2>Article not found</h2>';
} else {
    $post = blog_decode_post($encoded);
    $postTags = blog_load_post_tags($cache, [$post]);
    $content = blog_render_post($postTags, $post, true);
}
blog_render_page('Article', $content, $cache);

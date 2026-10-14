<?php

declare(strict_types=1);

require __DIR__ . '/bootstrap.php';

$redis = blog_redis();
$cache = blog_memcache();
$page = max(1, filter_input(INPUT_GET, 'page', FILTER_VALIDATE_INT) ?: 1);
$offset = ($page - 1) * POSTS_PER_PAGE;
$content = '<h2>Latest articles</h2>';
$posts = blog_load_posts($redis, 'zblog:posts', $offset, $offset + POSTS_PER_PAGE - 1);
$postTags = blog_load_post_tags($cache, $posts);
foreach ($posts as $post) {
    $content .= blog_render_post($postTags, $post);
}
blog_render_page('Home', $content, $cache);

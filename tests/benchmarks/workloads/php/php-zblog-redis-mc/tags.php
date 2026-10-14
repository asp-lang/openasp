<?php

declare(strict_types=1);

require __DIR__ . '/bootstrap.php';

$redis = blog_redis();
$cache = blog_memcache();
$selected = strtolower(trim((string)($_GET['tag'] ?? '')));
$content = '<h2>Tag cloud</h2><div>';
foreach (blog_metadata($cache, 'zblog:tags') as $tag) {
    $size = 13 + min(16, $tag['post_count']);
    $content .= '<a style="font-size:' . $size . 'px;margin-right:14px" href="/tags.php?tag=' .
        rawurlencode($tag['name']) . '">' . blog_escape($tag['name']) . '</a>';
}
$content .= '</div>';
if ($selected !== '') {
    $posts = blog_load_posts($redis, "zblog:tag:{$selected}", 0, 19);
    $postTags = blog_load_post_tags($cache, $posts);
    foreach ($posts as $post) {
        $content .= blog_render_post($postTags, $post);
    }
}
blog_render_page('Tags', $content, $cache);

<?php

declare(strict_types=1);

require __DIR__ . '/bootstrap.php';

$redis = blog_redis();
$posts = blog_load_posts($redis, 'zblog:posts', 0, 19);
header('Content-Type: application/rss+xml; charset=utf-8');
echo '<?xml version="1.0" encoding="UTF-8"?>';
echo '<rss version="2.0"><channel><title>', BLOG_NAME, '</title>';
echo '<link>http://localhost/default.php</link><description>Recent articles</description>';
foreach ($posts as $post) {
    echo '<item><title>', blog_escape((string)$post['title']), '</title>';
    echo '<link>http://localhost/view.php?id=', (int)$post['id'], '</link>';
    echo '<guid>benchmark-blog-', (int)$post['id'], '</guid>';
    echo '<pubDate>', blog_escape((string)$post['published_at']), '</pubDate>';
    echo '<description>', blog_escape((string)$post['summary']), '</description></item>';
}
echo '</channel></rss>';

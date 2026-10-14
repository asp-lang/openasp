<?php

declare(strict_types=1);

require __DIR__ . '/bootstrap.php';

$database = blog_db();
$page = max(1, filter_input(INPUT_GET, 'page', FILTER_VALIDATE_INT) ?: 1);
$offset = ($page - 1) * POSTS_PER_PAGE;
$statement = $database->prepare(
    "SELECT p.id, p.title, p.slug, p.summary, p.body, p.published_at, p.views,
            COALESCE(GROUP_CONCAT(t.name ORDER BY t.name SEPARATOR ','), '') AS tag_names
     FROM posts p
     LEFT JOIN post_tags pt ON pt.post_id = p.id
     LEFT JOIN tags t ON t.id = pt.tag_id
     GROUP BY p.id, p.title, p.slug, p.summary, p.body, p.published_at, p.views
     ORDER BY p.published_at DESC, p.id DESC
     LIMIT :limit OFFSET :offset"
);
$statement->bindValue('limit', POSTS_PER_PAGE, PDO::PARAM_INT);
$statement->bindValue('offset', $offset, PDO::PARAM_INT);
$statement->execute();

$content = '<h2>Latest articles</h2>';
foreach ($statement as $post) {
    $content .= blog_render_post($post);
}
blog_render_page('Home', $content, $database);

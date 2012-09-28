<?php

/**
 * @file
 * Default theme implementation for a link/button to update an issue.
 *
 * Available variables:
 * - $node: Full node object. Contains data that may not be safe.
 * - $classes_array: Array of html class attribute values. It is flattened
 *   into a string within the variable $classes.
 * - $update_url: String containing the URL for the update link.
 * - $update_text: Translated string for the main button text.
 * - $last_update: Text and formatted timestamp for the last issue update.
 *
 * @see template_preprocess_project_issue_issue_update_link()
 */
?>
<a href="<?php print $update_url;?>" class="<?php print $classes;?>">
  <div class="update-text"><?php print $update_text;?></div>
  <div class="last-update"><?php print $last_update;?></div>
</a>

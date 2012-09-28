<?php

/**
 * Form builder for the search box in the issue cockpit block.
 */
function project_issue_issue_cockpit_searchbox($form, &$form_state, $node) {
  $search_path = 'project/issues/' . $node->field_project_machine_name[LANGUAGE_NONE][0]['value'];
  $adv_search_path = 'project/issues/search/' . $node->field_project_machine_name[LANGUAGE_NONE][0]['value'];
  return array(
    '#action' => url($search_path),
    '#method' => 'get',
    'text' => array(
      '#type' => 'textfield',
      '#size' => 20,
    ),
    'status' => array(
      '#type' => 'hidden',
      '#value' => 'All',
    ),
    'submit' => array(
      '#type' => 'submit',
      '#value' => t('Search'),
      '#name' => '',
    ),
    'advanced' => array(
      '#markup' => l(t('Advanced search'), $adv_search_path),
    ),
  );
}

/**
 * Theme template preprocess to declare variables for the issue cockpit.
 */
function template_preprocess_project_issue_issue_cockpit(&$vars) {
  $node = $vars['node'];

  // Flags that indicate what kind of access to project issues to allow.
  $has_issues = !empty($node->field_project_has_issue_queue[LANGUAGE_NONE][0]['value']);
  $vars['view_issues'] = $has_issues && (user_access('access project issues') || user_access('access own project issues') || user_access('administer projects'));
  $vars['make_issues'] = $has_issues && node_access('create', 'project_issue');

  if ($vars['view_issues']) {

    $vars['path'] = 'project/issues/'. $node->field_project_machine_name[LANGUAGE_NONE][0]['value'];
    $vars['form'] = drupal_get_form('project_issue_issue_cockpit_searchbox', $node);

    $vars['categories'] = array();
    $categories = array('All' => t('All issues')) + project_issue_categories(TRUE);
    $setting = variable_get('project_issue_cockpit_categories', array('All' => 'All', 1 => 'Bug reports'));
    $active_categories = array_intersect_key($categories, array_filter($setting));
    foreach ($active_categories as $category => $name) {
      $vars['categories'][$category] = array('name' => $name, 'open' => 0, 'total' => 0);
    }

    $query = db_select('node', 'n');
    $query->innerJoin('field_data_field_project', 'p', "n.nid = p.entity_id AND p.entity_type = 'node'");
    $query->innerJoin('field_data_field_issue_category', 'c', "n.nid = c.entity_id AND c.entity_type = 'node'");
    // @todo Switch to using the "is an open issue" field during #1549428.
    //$query->innerJoin('field_data_field_issue_open', 'o', "n.nid = o.entity_id AND o.entity_type = 'node'");
    $query->innerJoin('field_data_field_issue_status', 's', "n.nid = s.entity_id AND s.entity_type = 'node'");
    $query->condition('n.status', 1);
    $query->condition('p.field_project_target_id', $node->nid);
    $query->addTag('node_access');

    if (isset($vars['categories']['All'])) {
      $temp = clone($query);
      // @todo Switch to using the "is an open issue" field during #1549428.
      //$temp->condition('o.field_issue_open_value', 1);
      $temp->condition('s.field_issue_status_value', project_issue_open_states());
      $vars['categories']['All']['open'] = $temp->countQuery()->execute()->fetchField();
      $temp = clone($query);
      $vars['categories']['All']['total'] = $temp->countQuery()->execute()->fetchField();
    }

    foreach ($active_categories as $category => $name) {
      if ($category == 'All') {
        continue;
      }
      $cquery = clone($query);
      $cquery->condition('c.field_issue_category_value', $category);

      $temp = clone($cquery);
      // @todo Switch to using the "is an open issue" field during #1549428.
      //$temp->condition('o.field_issue_open_value', 1);
      $temp->condition('s.field_issue_status_value', project_issue_open_states());

      $vars['categories'][$category]['open'] = $temp->countQuery()->execute()->fetchField();

      $temp = clone($cquery);
      $vars['categories'][$category]['total'] = $temp->countQuery()->execute()->fetchField();
    }

    $vars['issue_subscribe'] = l(t('Subscribe via e-mail'), 'project/issues/subscribe-mail/'. $node->field_project_machine_name[LANGUAGE_NONE][0]['value'], array('query' => drupal_get_destination()));
    if (user_access('access issue statistics')) {
      $vars['issue_statistics'] = l(t('Issue statistics'), 'project/issues/statistics/' . $node->field_project_machine_name[LANGUAGE_NONE][0]['value']);
    }

    // @todo Fix.
    // @todo Switch to using the "is an open issue" field during #1549428.
    //$vars['oldest'] = db_query("SELECT MIN(n.created) FROM {node} n INNER JOIN {field_data_field_issue_open} o ON n.nid = o.entity_id AND o.entity_type = 'node' INNER JOIN {field_data_field_project} p ON n.nid = p.entity_id AND p.entity_type = 'node' WHERE n.status = 1 AND o.field_issue_open_value = 1 AND n.type IN (:types) AND p.field_project_target_id = :project", array(':types' => project_issue_issue_node_types(), ':project' => $node->nid))->fetchField();
    $vars['oldest'] = db_query("SELECT MIN(n.created) FROM {node} n INNER JOIN {field_data_field_issue_status} s ON n.nid = s.entity_id AND s.entity_type = 'node' INNER JOIN {field_data_field_project} p ON n.nid = p.entity_id AND p.entity_type = 'node' WHERE n.status = 1 AND s.field_issue_status_value IN (:openstatus) AND n.type IN (:types) AND p.field_project_target_id = :project", array(':openstatus' => project_issue_open_states(), ':types' => project_issue_issue_node_types(), ':project' => $node->nid))->fetchField();
  }
}

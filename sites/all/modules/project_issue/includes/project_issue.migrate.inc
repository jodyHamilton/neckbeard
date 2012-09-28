<?php
/**
 * @file
 * Project issue D6 -> D7 migration.
 */

define('ISSUE_MIGRATE_TYPE_COMMENT', 0);
define('ISSUE_MIGRATE_TYPE_REVISION', 1);

define('ISSUE_MIGRATE_TIMELINE_OP', 0);
// Unfortunately, followups have to appear first so we have a semi-sane parent
// for existing revisions. The revisions will always appear later than reality,
// but since we don't know what reality IS due to revision corruption, there
// is not much we can do about it.
define('ISSUE_MIGRATE_TIMELINE_FOLLOWUP', 1);
define('ISSUE_MIGRATE_TIMELINE_REVISION', 2);
define('ISSUE_MIGRATE_TIMELINE_CURCHECK', 3);

// Uid used in cases of ambiguity -- Ownership of existing revisions is in doubt.
define('ISSUE_MIGRATE_SYSTEM_MIGRATION_USER', 180064); // Currently 'System Message'.

// Must not appear in file description field!
define('ISSUE_MIGRATE_FILE_DESCRIPTION_DELIMITER', '|||');
// Drupal.org specific!
define('ISSUE_MIGRATE_TAGS_VID', 9);

/**
 * Destination class implementing migration into node revisions.
 */
class ProjectIssueMigrateDestinationNodeRevision extends MigrateDestinationNode {
  static public function getKeySchema() {
    return array(
      'vid' => array(
        'type' => 'int',
        'unsigned' => TRUE,
        'description' => 'ID of destination revision',
      ),
    );
  }

  /**
   * Returns a list of fields available to be mapped for the node type (bundle)
   *
   * @param Migration $migration
   *  Optionally, the migration containing this destination.
   * @return array
   *  Keys: machine names of the fields (to be passed to addFieldMapping)
   *  Values: Human-friendly descriptions of the fields.
   */
  public function fields($migration = NULL) {
    $fields = parent::fields($migration);
    $fields['vid'] = t('Node: <a href="@doc">Revision (vid)</a>', array('@doc' => 'http://drupal.org/node/1298724'));
    return $fields;
  }

  /**
   * Delete a batch of node revisions at once.
   *
   * @param $vids
   *  Array of node revision IDs to be deleted.
   */
  public function bulkRollback(array $vids) {
    migrate_instrument_start('revision_delete_multiple');
    $this->prepareRollback($vids);
    foreach ($vids as $vid) {
      if ($revision = node_load(NULL, $vid)) {
        db_delete('node_revision')
          ->condition('vid', $revision->vid)
          ->execute();
        module_invoke_all('node_revision_delete', $revision);
        field_attach_delete_revision('node', $revision);
      }
    }
    $this->completeRollback($vids);
    migrate_instrument_stop('revision_delete_multiple');
  }

  /**
   * Import a single node revision.
   *
   * @param $node
   *  Node object to build. Prefilled with any fields mapped in the Migration.
   * @param $row
   *  Raw source data object - passed through to prepare/complete handlers.
   * @return array
   *  Array of key fields (vid only in this case) of the node that was saved if
   *  successful. FALSE on failure.
   */
  public function import(stdClass $node, stdClass $row) {
    // Updating previously-migrated content?
    $migration = Migration::currentMigration();

    if (method_exists($migration, 'prepareHack')) {
      $migration->prepareHack($node, $row);
    }

    if (isset($row->migrate_map_destid1)) {
      // Make sure is_new is off
      $node->is_new = FALSE;
      if (isset($node->vid)) {
        if ($node->vid != $row->migrate_map_destid1) {
          throw new MigrateException(t("Incoming vid !vid and map destination nid !destid1 don't match",
            array('!vid' => $node->vid, '!destid1' => $row->migrate_map_destid1)));
        }
      }
      else {
        $node->vid = $row->migrate_map_destid1;
      }
      // Get the existing tnid so updates don't generate notices
      $values = db_select('node', 'n')
                ->fields('n', array('tnid'))
                ->condition('nid', $node->nid)
                ->execute()
                ->fetchAssoc();
      if (empty($values)) {
        throw new MigrateException(t("Incoming node ID !nid no longer exists",
                    array('!nid' => $node->nid)));
      }
      if (empty($row->tnid)) {
        $node->tnid = $values['tnid'];
      }
    }
    if ($migration->getSystemOfRecord() == Migration::DESTINATION) {
      if (!isset($node->vid) && !isset($node->original)) {
        throw new MigrateException(t('System-of-record is DESTINATION, but no destination vid provided'));
      }
      if (!empty($node->original)) {
        $old_node = $node->original;
      }
      else {
        $old_node = node_load(NULL, $node->vid);
      }
      if (empty($old_node)) {
        throw new MigrateException(t('System-of-record is DESTINATION, but node with vid !vid does not exist',
                                   array('!vid' => $node->vid)));
      }
      if (!isset($node->created)) {
        $node->created = $old_node->created;
      }
      if (!isset($node->vid)) {
        // Creating new revision!
        $node->revision = 1;
      }
      if (!isset($node->status)) {
        $node->status = $old_node->status;
      }
      if (!isset($node->uid)) {
        $node->uid = $old_node->uid;
      }
    }
    elseif (!isset($node->type)) {
      // Default the type to our designated destination bundle (by doing this
      // conditionally, we permit some flexibility in terms of implementing
      // migrations which can affect more than one type).
      $node->type = $this->bundle;
    }

    // Set some required properties.

    if ($migration->getSystemOfRecord() == Migration::SOURCE) {
      if (!isset($node->language)) {
        $node->language = $this->language;
      }

      // Apply defaults, allow standard node prepare hooks to fire.
      // node_object_prepare() will blow these away, so save them here and
      // stuff them in later if need be.
      if (isset($node->created)) {
        $created = MigrationBase::timestamp($node->created);
      }
      else {
        // To keep node_object_prepare() from choking
        $node->created = REQUEST_TIME;
      }
      if (isset($node->changed)) {
        $changed = MigrationBase::timestamp($node->changed);
      }
      if (isset($node->uid)) {
        $uid = $node->uid;
      }
      node_object_prepare($node);
      if (isset($created)) {
        $node->created = $created;
      }
      // No point to resetting $node->changed here, node_save() will overwrite it
      if (isset($uid)) {
        $node->uid = $uid;
      }
    }

    // Invoke migration prepare handlers
    $this->prepare($node, $row);

    if (!isset($node->revision)) {
      $node->revision = 0; // Saves disk space and writes. Can be overridden.
    }

    // Trying to update an existing node
    if ($migration->getSystemOfRecord() == Migration::DESTINATION) {
      // Incoming data overrides existing data, so only copy non-existent fields
      foreach ($old_node as $field => $value) {
        // An explicit NULL in the source data means to wipe to old value (i.e.,
        // don't copy it over from $old_node)
        if (property_exists($node, $field) && $node->$field === NULL) {
          // Ignore this field
        }
        elseif (!isset($node->$field)) {
          $node->$field = $old_node->$field;
        }
      }
    }

    if (isset($node->vid) && !(isset($node->is_new) && $node->is_new)) {
      $updating = TRUE;
    }
    else {
      $updating = FALSE;
    }

    migrate_instrument_start('node_save');
    node_save($node);
    migrate_instrument_stop('node_save');

    if (isset($node->vid)) {
      if ($updating) {
        $this->numUpdated++;
      }
      else {
        $this->numCreated++;
      }

      // Unfortunately, http://drupal.org/node/722688 was not accepted, so fix
      // the changed timestamp
      if (isset($changed)) {
        db_update('node')
          ->fields(array('changed' => $changed))
          ->condition('nid', $node->nid)
          // Only update if the vid being updated is current.
          ->condition('vid', $node->vid)
          ->execute();
        $node->changed = $changed;
      }

      // Potentially fix uid and timestamp in node_revisions.
      $query = db_update('node_revision')
               ->condition('vid', $node->vid);
      if (isset($changed)) {
        $fields['timestamp'] = $changed;
      }
      $revision_uid = isset($node->revision_uid) ? $node->revision_uid : $node->uid;
      if ($revision_uid != $GLOBALS['user']->uid) {
        $fields['uid'] = $revision_uid;
      }
      if (!empty($fields)) {
        // We actually have something to update.
        $query->fields($fields);
        $query->execute();
        if (isset($changed)) {
          $node->timestamp = $changed;
        }
      }
      $return = array($node->vid);
    }
    else {
      $return = FALSE;
    }
    return $return;
  }
}

/**
 * Transfer initial uploads from upload to field_issue_files.
 */
class ProjectIssueFixInitFilesMigration extends MigrationBase {
  /**
   * General initialization of a ProjectIssueTimelineMigration object.
   */
  public function __construct($group = NULL) {
    parent::__construct($group);
    $this->description = t('WIP: USE AT YOUR OWN RISK! Move uploaded files to the correct field.');
  }

  protected function import() {
    db_query('INSERT INTO {field_data_field_issue_files} (SELECT * FROM {field_data_upload} WHERE entity_type = :type AND bundle = :bundle)', array(':type' => 'node', ':bundle' => 'project_issue'));
    db_query('INSERT INTO {field_revision_field_issue_files} (SELECT * FROM {field_revision_upload} WHERE entity_type = :type AND bundle = :bundle)', array(':type' => 'node', ':bundle' => 'project_issue'));
    db_query('DELETE FROM {field_data_upload} WHERE entity_type = :type AND bundle = :bundle', array(':type' => 'node', ':bundle' => 'project_issue'));
    db_query('DELETE FROM {field_revision_upload} WHERE entity_type = :type AND bundle = :bundle', array(':type' => 'node', ':bundle' => 'project_issue'));

    field_cache_clear();
  }

  public function isComplete() {
    $count = db_query('SELECT COUNT(*) FROM {field_data_upload} WHERE bundle = :bundle', array(':bundle' => 'project_issue'))->fetchField();
    return (bool) ($count == 0);
  }
}

/**
 * Transfer initial uploads from upload to field_issue_files.
 */
class ProjectIssueFixGenericCorruptionMigration extends MigrationBase {
  /**
   * General initialization of a ProjectIssueTimelineMigration object.
   */
  public function __construct($group = NULL) {
    parent::__construct($group);
    $this->description = t('Fix general drupal.org data problems.');
    $this->dependencies = array('ProjectIssuePhaseTwo');
  }

  protected function import() {
    // @@@ Ensure node.created is sane.

    // Wipe autogenerated comment subjects.
    db_query("UPDATE {comment} c, {project_issue_comments} i SET c.subject = '' WHERE c.subject = CONCAT('#', i.comment_number) AND c.cid = i.cid");
  }

  public function isComplete() {
//    $count = db_query('SELECT COUNT(*) FROM {field_data_upload} WHERE bundle = :bundle', array(':bundle' => 'project_issue'))->fetchField();
//    return (bool) ($count == 0);
  }
}

/**
 * Rethread project_issue comments to match comment_number.
 */
class ProjectIssueRethreadIssueFollowupsMigration extends MigrationBase {
  public function __construct($group = NULL) {
    parent::__construct($group);
    $this->description = t('WIP: USE AT YOUR OWN RISK! Rethread existing issue comments to match comment_number.');
    //$this->dependencies = array('ProjectIssuePhaseTwo');
  }

  protected function import() {
    if (db_table_exists('project_issue_migration_original_thread')) {
      db_drop_table('project_issue_migration_original_thread');
    }
    db_create_table('project_issue_migration_original_thread', array(
      'fields' => array(
        'cid' => array(
          'type' => 'int',
          'unsigned' => TRUE,
        ),
        'thread' => array(
          'type' => 'varchar',
          'length' => 255,
          'not null' => TRUE,
          'default' => '',
        ),
      ),
      'primary key' => array('cid'),
    ));
    // Store original threads so we can recover from mishaps.
    db_query("INSERT INTO {project_issue_migration_original_thread} (cid, thread) (SELECT cid, thread FROM {comment})");

    db_query("UPDATE {comment} c, {project_issue_comments} i SET c.thread = LOWER(CONCAT(CAST((LENGTH(CAST(CONV(i.comment_number, 10, 36) AS CHAR)) - 1) AS CHAR), CAST(CONV(i.comment_number, 10, 36) AS CHAR), '/')) WHERE c.cid = i.cid");
  }

  public function isComplete() {
    return db_table_exists('project_issue_migration_original_thread');
  }
}



/**
 * Special migration to assemble our full timeline.
 * Part 1: Make original_issue_data accessible by the system.
 * STATUS: DONE
 */
class ProjectIssueTimelinePhaseOneMigration extends MigrationBase {

  public function __construct($group = NULL) {
    parent::__construct($group);
    $this->description = t('Project issue timeline generator phase 1');
    $this->dependencies = array('ProjectIssueFixInitFiles');
  }

  protected function import() {
    if (db_table_exists('project_issue_migration_original_issue_data')) {
      db_drop_table('project_issue_migration_original_issue_data');
    }
    db_create_table('project_issue_migration_original_issue_data', array(
      'fields' => array(
        'nid' => array(
          'type' => 'int',
          'unsigned' => TRUE,
          'not null' => TRUE,
        ),
        'orig_vid' => array(
          'type' => 'int',
          'unsigned' => TRUE,
          'description' => 'Initial revision ID assigned when creating issue. (Assumed to be the lowest vid.)'
        ),
        'first_cid' => array(
          'type' => 'int',
          'unsigned' => TRUE,
          'description' => 'The cid of the first comment added to the issue node. This is needed for sanity checking.',
        ),
        'title' => array(
          'type' => 'varchar',
          'length' => 255,
          'not null' => FALSE, // Detect missing titles in serialized data...
          'default' => NULL,
        ),
        'pid' => array(
          'type' => 'int',
          'unsigned' => TRUE,
          'not null' => FALSE,
          'default' => NULL,
        ),
        'component' => array(
          'type' => 'varchar',
          'length' => 255,
          'not null' => FALSE,
          'default' => NULL,
        ),
        'category' => array(
          'type' => 'varchar',
          'length' => 255,
          'not null' => FALSE,
          'default' => NULL,
        ),
        'priority' => array(
          'type' => 'int',
          'unsigned' => TRUE,
          'not null' => FALSE,
          'default' => NULL,
        ),
        'assigned' => array(
          'type' => 'int',
          'unsigned' => TRUE,
          'not null' => FALSE,
          'default' => NULL,
        ),
        'sid' => array(
          'type' => 'int',
          'unsigned' => TRUE,
          'not null' => FALSE,
          'default' => NULL,
        ),
        'rid' => array(
          'type' => 'int',
          'unsigned' => TRUE,
          'not null' => FALSE,
          'default' => NULL,
        ),
      ),
      'primary key' => array('nid'),
      'indexes' => array(
        'orig_vid_idx' => array('orig_vid'),
      ),
    ));

    set_time_limit(0);
    // Step 1: Expand OP data.
    $insertQuery = db_insert('project_issue_migration_original_issue_data')
      ->fields(array('nid', 'orig_vid', 'first_cid', 'title', 'pid', 'component', 'category', 'priority', 'assigned', 'sid', 'rid'));

    // Expand in batches.
    $insertCounter = 0;
    $query = db_select('project_issues', 'p');
    $query->innerJoin('node_revision', 'r', 'p.nid = r.nid');
    $query->leftJoin('comment', 'c', 'p.nid = c.nid');
    $query->addField('p', 'nid');
    $query->addField('p', 'original_issue_data');
    $query->addExpression('MIN(r.vid)', 'orig_vid');
    $query->addExpression('MIN(c.cid)', 'first_cid');
    $query->groupBy('p.nid');
    $result = $query->execute();
    foreach ($result as $row) {
      $data = unserialize($row->original_issue_data);
      $data->nid = $row->nid;
      $insertQuery->values(array(
        'nid' => $data->nid,
        'orig_vid' => $row->orig_vid,
        'first_cid' => $row->first_cid,
        'title' => $data->title,
        'pid' => $data->pid,
        'component' => $data->component,
        'category' => $data->category,
        'priority' => $data->priority,
        'assigned' => $data->assigned,
        'sid' => $data->sid,
        'rid' => $data->rid,
      ));
      $insertCounter++;
      if ($insertCounter > 10000) {
        set_time_limit(0);
        $insertCounter = 0;
        // Flush the mulitple insert periodically.
        $insertQuery->execute();
      }
    }
    if ($insertCounter) {
      $insertQuery->execute();
    }
  }

  protected function rollback() {
    if (db_table_exists('project_issue_migration_original_issue_data')) {
      db_drop_table('project_issue_migration_original_issue_data');
    }
  }

  public function isComplete() {
    return db_table_exists('project_issue_migration_original_issue_data');
  }
}

/**
 * Special migration to assemble our full timeline.
 * Phase 2: Main table / OP injection
 * STATUS: IN PROGRESS
 */
class ProjectIssueTimelinePhaseTwoMigration extends MigrationBase {

  public function __construct($group = NULL) {
    parent::__construct($group);
    $this->description = t('WIP: USE AT YOUR OWN RISK! Project issue timeline generator phase 2');
    $this->dependencies = array('ProjectIssueTimelinePhaseOne');
  }

  protected function import() {
    if (db_table_exists('project_issue_migration_timeline')) {
      db_drop_table('project_issue_migration_timeline');
    }

    db_create_table('project_issue_migration_timeline', array(
      'fields' => array(
        'nid' => array(
          'type' => 'int',
          'unsigned' => TRUE,
          'not null' => TRUE,
        ),
        'prev_nid' => array(
          'type' => 'int',
          'unsigned' => TRUE,
          'description' => 'The nid of the previous row, used to speed up the fill algorithm.'
        ),
        'delta' => array(
          'type' => 'int',
          'unsigned' => TRUE,
          'not null' => TRUE,
          'default' => 0,
          'description' => 'The delta of this row on a per-node basis, used for the attachment algorithm.'
        ),
        'genid' => array(
          'type' => 'varchar',
          'length' => 255,
          'not null' => TRUE,
        ),
        'prev_genid' => array(
          'type' => 'varchar',
          'length' => 255,
        ),
        'prev_changed_genid' => array(
          'type' => 'varchar',
          'length' => 255,
          'description' => 'Previous genid that is considered changed.'
        ),
        'timestamp' => array(
          'type' => 'int',
          'unsigned' => TRUE,
          'not null' => TRUE,
        ),
        /*
        'prev_timestamp' => array(
          'type' => 'int',
          'unsigned' => TRUE,
        ),
        */
        'sort_timestamp' => array(
          'type' => 'int',
          'unsigned' => TRUE,
        ),
        'has_changes' => array(
          'type' => 'int',
          'unsigned' => TRUE,
          'size' => 'tiny',
          'not null' => TRUE,
          'default' => 0,
        ),
        'comment_number' => array(
          'type' => 'int',
          'unsigned' => TRUE,
        ),
        'vid' => array(
          'type' => 'int',
          'unsigned' => TRUE,
        ),
        'cid' => array(
          'type' => 'int',
          'unsigned' => TRUE,
        ),
        'tl' => array(
          'type' => 'int',
          'unsigned' => TRUE,
          'not null' => TRUE,
          'size' => 'tiny', // 8-bit is fine.
          'default' => ISSUE_MIGRATE_TIMELINE_OP,
          'description' => 'Role in timeline',
        ),
        'last_cid' => array(
          'type' => 'int',
          'unsigned' => TRUE,
          'description' => 'Previous cid, used to parent comments generated from existing revisions',
        ),
        // Project fields
        'cur_pid' => array(
          'type' => 'int',
          'unsigned' => TRUE,
          'description' => 'CURRENT project id',
        ),
        'prev_pid' => array(
          'type' => 'int',
          'unsigned' => TRUE,
          'description' => 'PREVIOUS project id',
        ),
        'cur_component' => array(
          'type' => 'varchar',
          'length' => 255,
          'description' => 'CURRENT component name',
        ),
        'prev_component' => array(
          'type' => 'varchar',
          'length' => 255,
          'description' => 'PREVIOUS component name'
        ),
        'cur_category' => array(
          'type' => 'varchar',
          'length' => 255,
          'description' => 'CURRENT category name'
        ),
        'prev_category' => array(
          'type' => 'varchar',
          'length' => 255,
          'description' => 'PREVIOUS category name'
        ),
        'cur_priority' => array(
          'type' => 'int',
          'unsigned' => TRUE,
          'description' => 'CURRENT priority',
        ),
        'prev_priority' => array(
          'type' => 'int',
          'unsigned' => TRUE,
          'description' => 'PREVIOUS priority',
        ),
        'cur_assigned' => array(
          'type' => 'int',
          'unsigned' => TRUE,
          'description' => 'CURRENT assigned',
        ),
        'prev_assigned' => array(
          'type' => 'int',
          'unsigned' => TRUE,
          'description' => 'PREVIOUS assigned',
        ),
        'cur_sid' => array(
          'type' => 'int',
          'unsigned' => TRUE,
          'description' => 'CURRENT state',
        ),
        'prev_sid' => array(
          'type' => 'int',
          'unsigned' => TRUE,
          'description' => 'PREVIOUS state',
        ),
        'cur_title' => array(
          'type' => 'varchar',
          'length' => 255,
          'description' => 'CURRENT title',
        ),
        'prev_title' => array(
          'type' => 'varchar',
          'length' => 255,
          'description' => 'PREVIOUS title',
        ),
        'cur_body_value' => array(
          'type' => 'text',
          'size' => 'big',
          'description' => 'CURRENT body value',
        ),
        'prev_body_value' => array(
          'type' => 'text',
          'size' => 'big',
          'description' => 'PREVIOUS body value',
        ),
        'cur_body_summary' => array(
          'type' => 'text',
          'size' => 'big',
          'description' => 'CURRENT body summary',
        ),
        'prev_body_summary' => array(
          'type' => 'text',
          'size' => 'big',
          'description' => 'PREVIOUS body summary',
        ),
        'cur_body_format' => array(
          'type' => 'varchar',
          'length' => 255,
          'description' => 'CURRENT body format',
        ),
        'prev_body_format' => array(
          'type' => 'varchar',
          'length' => 255,
          'description' => 'PREVIOUS body format',
        ),

        'cur_attachments' => array(
          'type' => 'text',
          'description' => 'CURRENT attachments (fid,display,description. triple pipe delimited.)',
        ),
        'prev_attachments' => array(
          'type' => 'text',
          'description' => 'PREVIOUS attachments (fid,display,description. triple pipe delimited.)',
        ),
        'cur_tags' => array(
          'type' => 'text',
          'description' => 'CURRENT vid 9 tids (comma separated)',
        ),
        'prev_tags' => array(
          'type' => 'text',
          'description' => 'PREVIOUS vid 9 tids (comma separated)',
        ),
        'comment_body_value' => array(
          'type' => 'text',
          'size' => 'big',
          'description' => 'Body value of comment.',
        ),
        'comment_body_format' => array(
          'type' => 'varchar',
          'length' => 255,
          'description' => 'Format of comment body.',
        ),
        'log' => array(
          'type' => 'text',
          'size' => 'big',
          'description' => 'Log entry for this row.',
        ),
        'uid' => array(
          'type' => 'int',
          'unsigned' => TRUE,
          'not null' => TRUE,
          'default' => 0,
        ),
      ),
      'primary key' => array('genid'),
      'indexes' => array(
        'prev_genid_idx' => array('prev_genid'),
        'timestamp_idx' => array('nid', 'timestamp'),
        // Use TL in sorts to ensure that followups and revisions don't happen
        // in the wrong order due to the revision timestamp corruption.
        'sort_idx' => array('nid', 'sort_timestamp', 'tl'),
        'vid_idx' => array('vid'),
        'cid_idx' => array('cid'),
        'delta_idx' => array('delta'),
        'nid_delta_idx' => array('nid', 'delta'),
      ),
    ));

    set_time_limit(0);
    // Serious MySQLism.
    db_query('SET SESSION group_concat_max_len = 1048576');

    // Copy OP 
    $query = db_select('project_issue_migration_original_issue_data', 'd');
    $query->innerJoin('node', 'n', 'd.nid = n.nid');
    $query->innerJoin('node_revision', 'r', 'r.vid = d.orig_vid');
    $query->innerJoin('field_revision_body', 'fb', 'r.vid = fb.revision_id AND fb.entity_type = :type', array(':type' => 'node'));
    $query->leftJoin('field_revision_field_issue_files', 'ff', 'r.vid = ff.revision_id AND ff.entity_type = :type', array(':type' => 'node'));
    $query->leftJoin('comment', 'c', 'd.first_cid = c.cid');
    // cid 0 is for the initial revision.
    // Since editing tags from the node edit form is inhibited, we can base our
    // tag conversion on {comment_alter_taxonomy} instead of worrying about
    // reconciling changes between it and the former {term_node} data.
    $query->leftJoin('comment_alter_taxonomy', 't', 'n.nid = t.nid AND t.cid = 0');

    $query->addField('d', 'nid');
    $query->addExpression('CONCAT(:vid, r.vid)', 'genid', array(':vid' => 'vid-'));
    $query->addField('r', 'vid');
    $query->addExpression("GROUP_CONCAT(CONCAT(ff.field_issue_files_fid, ',', ff.field_issue_files_display, ',', ff.field_issue_files_description) ORDER BY ff.delta ASC SEPARATOR :desc_delim)", 'cur_attachments', array(':desc_delim' => ISSUE_MIGRATE_FILE_DESCRIPTION_DELIMITER));
    $query->addExpression('IFNULL(GROUP_CONCAT(t.tid SEPARATOR :tags_delim), :tags_default)', 'cur_tags', array(':tags_default' => '', ':tags_delim' => ','));
    $query->addField('d', 'pid', 'cur_pid');
    $query->addField('d', 'component', 'cur_component');
    $query->addField('d', 'category', 'cur_category');
    $query->addField('d', 'priority', 'cur_priority');
    $query->addField('d', 'assigned', 'cur_assigned');
    $query->addField('d', 'sid', 'cur_sid');
    $query->addField('d', 'title', 'cur_title');
    $query->addExpression('LEAST(n.created, r.timestamp, c.created)', 'sort_timestamp'); // Due to trimming and corruption, node.created is sometimes faulty.
//    $query->addField('n', 'created', 'sort_timestamp'); // Sort goes by creation time to ensure original revision comes first.
    $query->addField('r', 'timestamp');
    $query->addField('r', 'log');
    $query->addField('r', 'uid');
    $query->addField('fb', 'body_value', 'cur_body_value');
    $query->addField('fb', 'body_summary', 'cur_body_summary');
    $query->addField('fb', 'body_format', 'cur_body_format');
    $query->addExpression(':tl', 'tl', array(':tl' => ISSUE_MIGRATE_TIMELINE_OP));

    $query->groupBy('d.nid');

    db_insert('project_issue_migration_timeline')
      ->from($query)
      ->execute();

    // Copy non-initial vids.
    $query = db_select('node_revision', 'r');
    $query->innerJoin('project_issues', 'p', 'r.nid = p.nid');
    $query->leftJoin('project_issue_migration_original_issue_data', 'v', 'r.vid = v.orig_vid');
    $query->innerJoin('field_revision_body', 'fb', 'r.vid = fb.revision_id AND fb.entity_type = :type', array(':type' => 'node'));
    $query->leftJoin('field_revision_field_issue_files', 'ff', 'r.vid = ff.revision_id AND ff.entity_type = :type', array(':type' => 'node'));
    // Ignore initial vid.
    $query->condition('v.orig_vid', NULL, 'IS NULL');

    $query->addField('r', 'nid');
    $query->addExpression('CONCAT(:vid, r.vid)', 'genid', array(':vid' => 'vid-'));
    $query->addField('r', 'vid');
    $query->addExpression("GROUP_CONCAT(CONCAT(ff.field_issue_files_fid, ',', ff.field_issue_files_display, ',', ff.field_issue_files_description) ORDER BY ff.delta ASC SEPARATOR :desc_delim)", 'cur_attachments', array(':desc_delim' => ISSUE_MIGRATE_FILE_DESCRIPTION_DELIMITER));
    $query->addField('r', 'timestamp');
    $query->addField('r', 'timestamp', 'sort_timestamp'); // Unfortunately, node revisions don't keep track of their creation time.
    $query->addField('r', 'log');
    $query->addField('r', 'uid');
    $query->addField('fb', 'body_value', 'cur_body_value');
    $query->addField('fb', 'body_summary', 'cur_body_summary');
    $query->addField('fb', 'body_format', 'cur_body_format');
    $query->addExpression(':tl', 'tl', array(':tl' => ISSUE_MIGRATE_TIMELINE_REVISION));

    $query->groupBy('r.vid');

    db_insert('project_issue_migration_timeline')
      ->from($query)
      ->execute();

    // Copy cids.
    $query = db_select('project_issue_comments', 'p');
    $query->innerJoin('comment', 'c', 'p.cid = c.cid');
    $query->leftJoin('comment_upload', 'cu', 'p.cid = cu.cid');
    $query->innerJoin('field_data_comment_body', 'fb', 'p.cid = fb.entity_id AND fb.entity_type = :type', array(':type' => 'comment'));
    $query->leftJoin('comment_alter_taxonomy', 't', 'p.nid = t.nid AND p.cid = t.cid');

    $query->addField('p', 'nid');
    $query->addExpression('CONCAT(:cid, p.cid)', 'genid', array(':cid' => 'cid-'));
    $query->addField('p', 'cid');
    $query->addExpression("GROUP_CONCAT(CONCAT(cu.fid, ',', cu.list, ',', cu.description) ORDER BY cu.weight ASC SEPARATOR :desc_delim)", 'cur_attachments', array(':desc_delim' => ISSUE_MIGRATE_FILE_DESCRIPTION_DELIMITER));
    $query->addExpression('IFNULL(GROUP_CONCAT(t.tid SEPARATOR :tags_delim), :tags_default)', 'cur_tags', array(':tags_default' => '', ':tags_delim' => ','));
    $query->addField('p', 'pid', 'cur_pid');
    $query->addField('p', 'component', 'cur_component');
    $query->addField('p', 'category', 'cur_category');
    $query->addField('p', 'priority', 'cur_priority');
    $query->addField('p', 'assigned', 'cur_assigned');
    $query->addField('p', 'sid', 'cur_sid');
    $query->addField('p', 'title', 'cur_title');
    $query->addField('p', 'comment_number');
    $query->addField('c', 'changed', 'timestamp');
    $query->addField('c', 'created', 'sort_timestamp'); // Sort goes by creation time to ensure original revision comes first.
    //$query->addField('fb', 'comment_body_value', 'log');
    $query->addField('fb', 'comment_body_value', 'comment_body_value');
    $query->addField('fb', 'comment_body_format', 'comment_body_format');
    $query->addField('c', 'uid');
    $query->addExpression(':tl', 'tl', array(':tl' => ISSUE_MIGRATE_TIMELINE_FOLLOWUP));

    $query->groupBy('p.cid');

    db_insert('project_issue_migration_timeline')
      ->from($query)
      ->execute();
  }

  protected function rollback() {
    if (db_table_exists('project_issue_migration_timeline')) {
      db_drop_table('project_issue_migration_timeline');
    }
  }

  public function isComplete() {
    return db_table_exists('project_issue_migration_timeline');
  }
}

/**
 * Special migration to assemble our full timeline.
 * Phase 2: Main table / OP injection
 * STATUS: IN PROGRESS
 */
class ProjectIssueTimelinePhaseThreeMigration extends MigrationBase {

  public function __construct($group = NULL) {
    parent::__construct($group);
    $this->description = t('WIP: USE AT YOUR OWN RISK! Project issue timeline generator phase 3');
    $this->dependencies = array('ProjectIssueTimelinePhaseTwo');
  }

  protected function import() {
    set_time_limit(0);
    // Serious MySQLism.
    db_query('SET SESSION group_concat_max_len = 1048576');

    $min = 0;
    $max = db_query('SELECT MAX(nid) FROM {node} WHERE type = :type', array(':type' => 'project_issue'))->fetchField();

    db_query("DROP PROCEDURE IF EXISTS project_issue_migrator");
    db_query("CREATE PROCEDURE project_issue_migrator(IN nid_min INT UNSIGNED, IN nid_max INT UNSIGNED)
BEGIN
  DECLARE done INT DEFAULT FALSE;
  DECLARE a INT DEFAULT 0;
  DECLARE b INT DEFAULT 1;
  DECLARE firstline TINYINT UNSIGNED DEFAULT 1;

  DECLARE c_delta INT DEFAULT 0;

  DECLARE c_nid INT UNSIGNED;
  DECLARE p_nid INT UNSIGNED;
  DECLARE c_genid VARCHAR(255) CHARACTER SET utf8;
  DECLARE p_genid VARCHAR(255) CHARACTER SET utf8;
  DECLARE p_changed_genid VARCHAR(255) CHARACTER SET utf8;
  DECLARE c_lastcid INT UNSIGNED;
  DECLARE c_cid INT UNSIGNED;
  DECLARE c_haschanges TINYINT UNSIGNED;
  DECLARE c_tl TINYINT UNSIGNED;

  DECLARE c_body_value LONGTEXT CHARACTER SET utf8;
  DECLARE p_body_value LONGTEXT CHARACTER SET utf8;
  DECLARE c_body_summary LONGTEXT CHARACTER SET utf8;
  DECLARE p_body_summary LONGTEXT CHARACTER SET utf8;
  DECLARE c_body_format VARCHAR(255) CHARACTER SET utf8;
  DECLARE p_body_format VARCHAR(255) CHARACTER SET utf8;
  DECLARE c_pid INT UNSIGNED;
  DECLARE p_pid INT UNSIGNED;
  DECLARE c_component VARCHAR(255) CHARACTER SET utf8;
  DECLARE p_component VARCHAR(255) CHARACTER SET utf8;
  DECLARE c_category VARCHAR(255) CHARACTER SET utf8;
  DECLARE p_category VARCHAR(255) CHARACTER SET utf8;
  DECLARE c_priority INT UNSIGNED;
  DECLARE p_priority INT UNSIGNED;
  DECLARE c_assigned INT UNSIGNED;
  DECLARE p_assigned INT UNSIGNED;
  DECLARE c_sid INT UNSIGNED;
  DECLARE p_sid INT UNSIGNED;
  DECLARE c_title VARCHAR(255) CHARACTER SET utf8;
  DECLARE p_title VARCHAR(255) CHARACTER SET utf8;
  DECLARE c_tags TEXT CHARACTER SET utf8;
  DECLARE p_tags TEXT CHARACTER SET utf8;
  DECLARE x_tags TEXT CHARACTER SET utf8;
  DECLARE c_attachments TEXT CHARACTER SET utf8;
  DECLARE p_attachments TEXT CHARACTER SET utf8;

  DECLARE tl_cur CURSOR FOR
    SELECT nid, genid, cid, tl, cur_body_value, cur_body_summary, cur_body_format, cur_pid, cur_component, cur_category, cur_priority, cur_assigned, cur_sid, cur_title, cur_tags, cur_attachments
    FROM {project_issue_migration_timeline}
    WHERE nid BETWEEN nid_min AND nid_max
    ORDER BY nid ASC, sort_timestamp ASC, tl ASC;
  DECLARE CONTINUE HANDLER FOR NOT FOUND SET done = TRUE;

  OPEN tl_cur;

  read_loop: LOOP
    FETCH tl_cur INTO c_nid, c_genid, c_cid, c_tl, c_body_value, c_body_summary, c_body_format, c_pid, c_component, c_category, c_priority, c_assigned, c_sid, c_title, x_tags, c_attachments;
    IF done THEN
      LEAVE read_loop;
    END IF;
    IF c_nid <> p_nid THEN

      SET p_genid = NULL;
      SET c_delta = 0;

      SET p_body_value    = NULL;
      SET p_body_summary  = NULL;
      SET p_body_format   = NULL;
      SET p_pid           = NULL;
      SET p_component     = NULL;
      SET p_category      = NULL;
      SET p_priority      = NULL;
      SET p_assigned      = NULL;
      SET p_sid           = NULL;
      SET p_title         = NULL;
      SET p_tags          = NULL;
      SET p_attachments   = NULL;

      SET c_lastcid       = NULL;
      SET p_changed_genid = NULL;

      SET c_tags          = x_tags;

    END IF;

    IF c_cid IS NOT NULL THEN
      SET c_lastcid = c_cid;
      SET c_tags = x_tags;
    END IF;

    IF c_attachments IS NULL THEN
      SET c_attachments = p_attachments;
    ELSEIF c_attachments <> p_attachments AND p_attachments IS NOT NULL THEN
      SET c_attachments = CONCAT(p_attachments, '" . ISSUE_MIGRATE_FILE_DESCRIPTION_DELIMITER . "', c_attachments);
    END IF;

    SET c_body_value   = IFNULL(c_body_value,   p_body_value);
    SET c_body_summary = IFNULL(c_body_summary, p_body_summary);
    SET c_body_format  = IFNULL(c_body_format,  p_body_format);
    SET c_pid          = IFNULL(c_pid,          p_pid);
    SET c_component    = IFNULL(c_component,    p_component);
    SET c_category     = IFNULL(c_category,     p_category);
    SET c_priority     = IFNULL(c_priority,     p_priority);
    SET c_assigned     = IFNULL(c_assigned,     p_assigned);
    SET c_sid          = IFNULL(c_sid,          p_sid);
    SET c_title        = IFNULL(c_title,        p_title);

    IF c_pid <> p_pid
        OR c_component <> p_component
        OR c_category <> p_category
        OR c_priority <> p_priority
        OR c_assigned <> p_assigned
        OR c_sid <> p_sid
        OR c_title <> p_title
        OR c_body_value <> p_body_value
        OR c_body_summary <> p_body_summary
        OR c_body_format <> p_body_format
        OR c_attachments <> p_attachments
        OR c_tags <> p_tags
        OR c_tl = ". ISSUE_MIGRATE_TIMELINE_OP . " THEN
      SET c_haschanges = 1;
    ELSE
      SET c_haschanges = 0;
    END IF;

    UPDATE {project_issue_migration_timeline}
      SET prev_nid           = p_nid,
          prev_genid         = p_genid,
          delta              = c_delta,
          has_changes        = c_haschanges,
          prev_changed_genid = p_changed_genid,
          prev_body_value    = p_body_value,
          cur_body_value     = c_body_value,
          prev_body_summary  = p_body_summary,
          cur_body_summary   = c_body_summary,
          prev_body_format   = p_body_format,
          cur_body_format    = c_body_format,
          prev_pid           = p_pid,
          cur_pid            = c_pid,
          prev_component     = p_component,
          cur_component      = c_component,
          prev_category      = p_category,
          cur_category       = c_category,
          prev_priority      = p_priority,
          cur_priority       = c_priority,
          prev_assigned      = p_assigned,
          cur_assigned       = c_assigned,
          prev_sid           = p_sid,
          cur_sid            = c_sid,
          prev_title         = p_title,
          cur_title          = c_title,
          prev_tags          = p_tags,
          cur_tags           = c_tags,
          prev_attachments   = p_attachments,
          cur_attachments    = c_attachments,
          last_cid           = c_lastcid
      WHERE genid = c_genid;

    SET p_nid = c_nid;
    SET c_delta = c_delta + 1;
    SET p_genid = c_genid;

    SET p_body_value   = c_body_value;
    SET p_body_summary = c_body_summary;
    SET p_body_format  = c_body_format;
    SET p_pid          = c_pid;
    SET p_component    = c_component;
    SET p_category     = c_category;
    SET p_priority     = c_priority;
    SET p_assigned     = c_assigned;
    SET p_sid          = c_sid;
    SET p_title        = c_title;
    SET p_tags         = c_tags;
    SET p_attachments  = c_attachments;

    IF c_haschanges = 1 THEN
      SET p_changed_genid = c_genid;
    END IF;

  END LOOP;

  CLOSE tl_cur;
END");
    while ($min <= $max) {
      $gmax = $min + 5000;
      // sleep(2); // Drupal.org "play nice with the database" call.
      db_query("CALL project_issue_migrator(:gmin, :gmax)", array(':gmin' => $min, ':gmax' => $gmax));
      set_time_limit(0);
      $min = $gmax;
    }
  }

  protected function rollback() {
    if (db_table_exists('project_issue_migration_timeline')) {
      db_drop_table('project_issue_migration_timeline');
    }
  }

  public function isComplete() {
    return db_table_exists('project_issue_migration_timeline');
  }
}

/**
 * Convert comments and existing revisions.
 */
class ProjectIssuePhaseTwoMigration extends Migration {
  public function __construct() {
    // We are loading from the destination system.
    $this->systemOfRecord = Migration::DESTINATION;

    parent::__construct();
    $this->description = t('WIP: USE AT YOUR OWN RISK! Project issue migration (Phase 2)');
    $this->dependencies = array('ProjectIssueTimelinePhaseThree', 'ProjectIssueRethreadIssueFollowups');

    // We are loading from the destination system.
    $this->systemOfRecord = Migration::DESTINATION;


    require_once('includes/database/database.inc');

    $query = db_select('project_issue_migration_timeline', 't');
    $query->fields('t');
    $query->condition(db_or()
      ->condition('has_changes', 1)
      // All revisions are resaved so they acquire the correct field values.
      // Comments that have no changes are skipped, as we can treat them as
      // "normal" comments.
      ->condition('tl', ISSUE_MIGRATE_TIMELINE_REVISION));
    $query->orderBy('nid', 'ASC');
    $query->orderBy('sort_timestamp', 'ASC');
    $query->orderBy('tl', 'ASC');

    $this->source = new MigrateSourceSQL($query, array('cur_attachment_fids' => t('Attachment fids'), 'cur_attachment_display' => t('Attachment display booleans'), 'cur_attachment_description' => t('Attachment descriptions')));
    // Migrate into node revisions.
    $this->destination = new ProjectIssueMigrateDestinationNodeRevision('project_issue');

    // We instantiate the MigrateMap
    $this->map = new MigrateSQLMap($this->machineName,
        array(
          'genid' => array(
            'type' => 'varchar',
            'length' => 255,
            'not null' => TRUE,
          ),
        ),
        ProjectIssueMigrateDestinationNodeRevision::getKeySchema()
      );

    // Finally we add simple field mappings
    $this->addFieldMapping('nid', 'nid');

    $this->addFieldMapping('revision_uid', 'uid');
    $this->addFieldMapping('changed', 'timestamp');
    $this->addFieldMapping('title', 'cur_title');

    $this->addFieldMapping('body', 'cur_body_value');
    $this->addFieldMapping('body:format', 'cur_body_format');
    $this->addFieldMapping('body:summary', 'cur_body_summary');

    $this->addFieldMapping('field_issue_status', 'cur_sid');
    $this->addFieldMapping('field_issue_priority', 'cur_priority');
    $this->addFieldMapping('field_issue_category', 'cur_category')
      ->callbacks('_project_issue_map_category');
    $this->addFieldMapping('field_issue_component', 'cur_component');
    $this->addFieldMapping('field_issue_assigned', 'cur_assigned');
    $this->addFieldMapping('field_project', 'cur_pid');

    $this->addFieldMapping('field_issue_files', 'cur_attachment_fids')
      ->separator(',');
    $this->addFieldMapping('field_issue_files:file_class')
      ->defaultValue('MigrateFileFid');
//    $this->addFieldMapping('field_issue_files:preserve_files')
//      ->defaultValue(TRUE);
    $this->addFieldMapping('field_issue_files:display', 'cur_attachment_display')
      ->separator(',');
    $this->addFieldMapping('field_issue_files:description', 'cur_attachment_description')
      ->separator(ISSUE_MIGRATE_FILE_DESCRIPTION_DELIMITER);

    $this->addFieldMapping('is_new')
      ->defaultValue(0);
  }

  public function preImport() {
    // Make sure ip_address has its static allocated.
    ip_address();
  }

  public function prepareHack($node, $row) {
    $node->is_new = FALSE;
    // Use the correct revision for the node original state so the diffs are
    // relative to it instead of the most recent revision.
    if ($row->tl == ISSUE_MIGRATE_TIMELINE_OP) {
      // The OP is always compared against itself.
      $node->original = node_load(NULL, $row->vid, TRUE);
    }
    else {
      $vid = $this->getMap()->lookupDestinationID(array($row->prev_changed_genid));
      if ($vid) {
        $node->original = node_load(NULL, reset($vid), TRUE);
      }
      else {
        print_r($row);
      }
    }
    if ($row->vid) {
      $node->vid = $row->vid;
    }
  }

  public function prepareRow($row) {
    $return = parent::prepareRow($row);
    $fids = array();
    $disps = array();
    $descs = array();
    // Sometimes they stack due to the nature of the queries we use to generate
    // the fid list.
    $found = array();
    if (!empty($row->cur_attachments)) {
      $files = explode(ISSUE_MIGRATE_FILE_DESCRIPTION_DELIMITER, $row->cur_attachments);
      foreach ($files as $file) {
        list($fid, $disp, $desc) = explode(',', $file, 3);
        if (!isset($found[$fid])) {
          $found[$fid] = TRUE;
          $fids[] = $fid;
          $disps[] = $disp;
          $descs[] = $desc;
        }
      }
    }
    $row->cur_attachment_fids = implode(',', $fids);
    $row->cur_attachment_display = implode(',', $disps);
    $row->cur_attachment_description = implode(ISSUE_MIGRATE_FILE_DESCRIPTION_DELIMITER, $descs);
    return $return;
  }

  public function prepare(stdClass $node, stdClass $row) {
    if (!empty($row->log)) {
      // Prevent node_save from using the previous revision's log as a basis
      // due to the fact that we are using the *previous* revision as our
      // "original" to simulate making the changes.
      $node->log = $row->log;
    }
    if ($row->tl == ISSUE_MIGRATE_TIMELINE_OP || !$row->has_changes) {
      // While we are resaving everything but unchanged comments, unchanged
      // revisions do NOT get a nodechanges comment.
      $node->nodechanges_skip = TRUE;
      // But continue to use migrate to fix up the timestamp.
      $node->_project_issue_changed_timestamp = $row->timestamp;
    }
    if ($row->cid) {
      // Comment to revision conversion.
      $node->nodechanges_cid = $row->cid;
      // Ensure new revision is created.
      $node->revision = TRUE;
      $node->_project_issue_changed_timestamp = $row->timestamp;
    }
    else {
      // Resaving existing revision.
      $node->vid = $row->vid;
      $node->revision = FALSE;
      if ($row->last_cid) {
        // Set parent for new comment.
        $node->nodechanges_pid = $row->last_cid;
      }
      $comment = new stdClass;
      $comment->status = COMMENT_PUBLISHED;
      $comment->uid = ISSUE_MIGRATE_SYSTEM_MIGRATION_USER;
      // @todo Maybe we should use an artificial timestamp like 0 or 280281600.
      $comment->created = $row->timestamp;
      $comment->changed = $row->timestamp;
      $node->nodechanges_comment = $comment;
      // @todo Boilerplate text.
      $node->nodechanges_body = $row->comment_body_value;
      $node->nodechanges_body_format = $row->comment_body_format;
      $node->nodechanges_uid = ISSUE_MIGRATE_SYSTEM_MIGRATION_USER;
    }

    global $conf;
    // Lock out _comment_update_node_statistics().
    $conf['comment_maintain_node_statistics'] = FALSE;
  }

  public function complete(stdClass $node, stdClass $row) {
  }
}

/**
 * Map category names to integers.
 */
function _project_issue_map_category($value) {
  switch ($value) {
    case 'feature':
      return 3;
    case 'bug':
      return 1;
    case 'support':
      return 4;
    case 'task':
      return 2;
    default:
      return 0;
  }
}

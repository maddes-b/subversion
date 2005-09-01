require "my-assertions"
require "util"

require "svn/core"
require "svn/fs"
require "svn/repos"
require "svn/client"

class SvnReposTest < Test::Unit::TestCase
  include SvnTestUtil
  
  def setup
    setup_basic
  end

  def teardown
    teardown_basic
  end

  def test_version
    assert_equal(Svn::Core.subr_version, Svn::Repos.version)
  end

  def test_path
    assert_equal(@repos_path, @repos.path)

    assert_equal(File.join(@repos_path, "db"), @repos.db_env)

    assert_equal(File.join(@repos_path, "conf"), @repos.conf_dir)
    assert_equal(File.join(@repos_path, "conf", "svnserve.conf"),
                 @repos.svnserve_conf)
    
    locks_dir = File.join(@repos_path, "locks")
    assert_equal(locks_dir, @repos.lock_dir)
    assert_equal(File.join(locks_dir, "db.lock"),
                 @repos.db_lockfile)
    assert_equal(File.join(locks_dir, "db-logs.lock"),
                 @repos.db_logs_lockfile)

    hooks_dir = File.join(@repos_path, "hooks")
    assert_equal(hooks_dir, @repos.hook_dir)
    
    assert_equal(File.join(hooks_dir, "start-commit"),
                 @repos.start_commit_hook)
    assert_equal(File.join(hooks_dir, "pre-commit"),
                 @repos.pre_commit_hook)
    assert_equal(File.join(hooks_dir, "post-commit"),
                 @repos.post_commit_hook)
    
    assert_equal(File.join(hooks_dir, "pre-revprop-change"),
                 @repos.pre_revprop_change_hook)
    assert_equal(File.join(hooks_dir, "post-revprop-change"),
                 @repos.post_revprop_change_hook)

    assert_equal(File.join(hooks_dir, "pre-lock"),
                 @repos.pre_lock_hook)
    assert_equal(File.join(hooks_dir, "post-lock"),
                 @repos.post_lock_hook)

    assert_equal(File.join(hooks_dir, "pre-unlock"),
                 @repos.pre_unlock_hook)
    assert_equal(File.join(hooks_dir, "post-unlock"),
                 @repos.post_unlock_hook)

    
    search_path = @repos_path
    assert_equal(@repos_path, Svn::Repos.find_root_path(search_path))
    search_path = "#{@repos_path}/XXX"
    assert_equal(@repos_path, Svn::Repos.find_root_path(search_path))

    search_path = "not-found"
    assert_equal(nil, Svn::Repos.find_root_path(search_path))
  end

  def test_create
    tmp_repos_path = File.join(@tmp_path, "repos")
    fs_config = {Svn::Fs::CONFIG_FS_TYPE => Svn::Fs::TYPE_BDB}
    Svn::Repos.create(tmp_repos_path, {}, fs_config)
    assert(File.exist?(tmp_repos_path))
    repos = Svn::Repos.open(tmp_repos_path)
    fs_type_path = File.join(repos.fs.path, Svn::Fs::CONFIG_FS_TYPE)
    assert_equal(Svn::Fs::TYPE_BDB,
                 File.open(fs_type_path) {|f| f.read.chop})
    repos = nil
    GC.start
    Svn::Repos.delete(tmp_repos_path)
    assert(!File.exist?(tmp_repos_path))
  end

  def test_hotcopy
    log = "sample log"
    file = "hello.txt"
    path = File.join(@wc_path, file)
    FileUtils.touch(path)
    
    ctx = make_context(log)
    ctx.add(path)
    commit_info = ctx.commit(@wc_path)
    rev = commit_info.revision
    
    assert_equal(log, ctx.log_message(path, rev))
    
    dest_path = File.join(@tmp_path, "dest")
    backup_path = File.join(@tmp_path, "back")
    config = {}
    fs_config = {}

    repos = Svn::Repos.create(dest_path, config, fs_config)

    FileUtils.mv(@repos.path, backup_path)
    FileUtils.mv(repos.path, @repos.path)

    assert_raises(Svn::Error::FS_NO_SUCH_REVISION) do
      assert_equal(log, ctx.log_message(path, rev))
    end

    FileUtils.rm_r(@repos.path)
    Svn::Repos.hotcopy(backup_path, @repos.path)
    assert_equal(log, ctx.log_message(path, rev))
  end
  
  def test_transaction
    log = "sample log"
    ctx = make_context(log)
    ctx.checkout(@repos_uri, @wc_path)
    ctx.mkdir(["#{@wc_path}/new_dir"])
    
    prev_rev = @repos.youngest_rev
    past_date = Time.now
    @repos.transaction_for_commit(@author, log) do |txn|
      txn.abort
    end
    assert_equal(prev_rev, @repos.youngest_rev)
    assert_equal(prev_rev, @repos.dated_revision(past_date))
    
    prev_rev = @repos.youngest_rev
    @repos.transaction_for_commit(@author, log) do |txn|
    end
    assert_equal(prev_rev + 1, @repos.youngest_rev)
    assert_equal(prev_rev, @repos.dated_revision(past_date))
    assert_equal(prev_rev + 1, @repos.dated_revision(Time.now))
  end

  def test_report
    file = "file"
    path = File.join(@wc_path, file)
    source = "sample source"
    log = "sample log"
    ctx = make_context(log)

    File.open(path, "w") {|f| f.print(source)}
    ctx.add(path)
    rev = ctx.ci(@wc_path).revision

    assert_equal(Svn::Core::NODE_FILE, @repos.fs.root.stat(file).kind)

    assert_raise(Svn::Error::REPOS_BAD_REVISION_REPORT) do
      @repos.report(rev, @author, @repos_path,
                    @wc_path, "/", Svn::Delta::BaseEditor.new) do |baton|
      end
    end
  end

  def test_commit_editor
    trunk = "trunk"
    tags = "tags"
    tags_sub = "sub"
    file = "file"
    source = "sample source"
    trunk_dir_path = File.join(@wc_path, trunk)
    tags_dir_path = File.join(@wc_path, tags)
    tags_sub_dir_path = File.join(tags_dir_path, tags_sub)
    trunk_path = File.join(trunk_dir_path, file)
    tags_path = File.join(tags_dir_path, file)
    tags_sub_path = File.join(tags_sub_dir_path, file)
    trunk_repos_uri = "#{@repos_uri}/#{trunk}"
    rev1 = @repos.youngest_rev
    
    editor = @repos.commit_editor(@repos_uri, "/")
    root_baton = editor.open_root(rev1)
    dir_baton = editor.add_directory(trunk, root_baton, nil, rev1)
    file_baton = editor.add_file("#{trunk}/#{file}", dir_baton, nil, -1)
    ret = editor.apply_textdelta(file_baton, nil)
    ret.send(source)
    editor.close_edit
    
    assert_equal(rev1 + 1, @repos.youngest_rev)
    rev2 = @repos.youngest_rev
    
    ctx = make_context("")
    ctx.up(@wc_path)
    assert_equal(source, File.open(trunk_path) {|f| f.read})

    editor = @repos.commit_editor(@repos_uri, "/")
    root_baton = editor.open_root(rev2)
    dir_baton = editor.add_directory(tags, root_baton, nil, rev2)
    subdir_baton = editor.add_directory("#{tags}/#{tags_sub}",
                                        dir_baton,
                                        trunk_repos_uri,
                                        rev2)
    editor.close_edit
    
    assert_equal(rev2 + 1, @repos.youngest_rev)
    rev3 = @repos.youngest_rev
    
    ctx.up(@wc_path)
    assert_equal([
                   ["/#{tags}/#{tags_sub}/#{file}", rev3],
                   ["/#{trunk}/#{file}", rev2],
                 ],
                 @repos.fs.history("#{tags}/#{tags_sub}/#{file}",
                                   rev1, rev3, rev2))

    editor = @repos.commit_editor(@repos_uri, "/")
    root_baton = editor.open_root(rev3)
    dir_baton = editor.delete_entry(tags, rev3, root_baton)
    editor.close_edit

    ctx.up(@wc_path)
    assert(!File.exist?(tags_path))
  end
end

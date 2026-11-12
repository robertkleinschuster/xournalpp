#include "ProjectGit.h"

#include <cstring>
#include <future>
#include <memory>
#include <set>
#include <utility>

#include <git2.h>
#include <glib.h>

#include "control/AnnotationFiles.h"  // for isRecoveryCopy
#include "util/PathUtil.h"            // for toUtf8, fromUtf8

#ifdef _WIN32
#include <windows.h>

#include <wincred.h>  // after windows.h
#endif

namespace fs = std::filesystem;
using namespace ProjectGit;

namespace {

// --- libgit2 basics ---

void ensureInitialized() {
    static const bool initialized = [] {
        git_libgit2_init();
        // Do not wait minutes for an unreachable server (git's own defaults are unlimited)
        git_libgit2_opts(GIT_OPT_SET_SERVER_CONNECT_TIMEOUT, 30000);
        git_libgit2_opts(GIT_OPT_SET_SERVER_TIMEOUT, 120000);
        return true;
    }();
    (void)initialized;
}

template <typename T, void (*Free)(T*)>
struct Deleter {
    void operator()(T* p) const {
        if (p) {
            Free(p);
        }
    }
};
template <typename T, void (*Free)(T*)>
using Ptr = std::unique_ptr<T, Deleter<T, Free>>;

using Repo = Ptr<git_repository, git_repository_free>;
using Ref = Ptr<git_reference, git_reference_free>;
using Index = Ptr<git_index, git_index_free>;
using Commit = Ptr<git_commit, git_commit_free>;
using Tree = Ptr<git_tree, git_tree_free>;
using Remote = Ptr<git_remote, git_remote_free>;
using StatusList = Ptr<git_status_list, git_status_list_free>;
using Signature = Ptr<git_signature, git_signature_free>;
using Annotated = Ptr<git_annotated_commit, git_annotated_commit_free>;
using Rebase = Ptr<git_rebase, git_rebase_free>;
using Object = Ptr<git_object, git_object_free>;
using Blob = Ptr<git_blob, git_blob_free>;

auto lastError() -> std::string {
    const git_error* error = git_error_last();
    return error && error->message ? error->message : "";
}

auto failed(std::string message = lastError()) -> Result { return {Outcome::Failed, std::move(message)}; }

/// Holds the strings of a git_strarray
struct StrArray {
    explicit StrArray(std::vector<std::string> values): strings(std::move(values)) {
        for (auto& s: strings) {
            pointers.push_back(s.data());
        }
        array.strings = pointers.data();
        array.count = pointers.size();
    }
    std::vector<std::string> strings;
    std::vector<char*> pointers;
    git_strarray array{};
};

auto openRepo(const fs::path& dir) -> std::pair<Repo, int> {
    ensureInitialized();
    git_repository* repo = nullptr;
    const int error = git_repository_open_ext(&repo, Util::toUtf8(dir).c_str(), 0, nullptr);
    return {Repo(repo), error};
}

auto workdirOf(git_repository* repo) -> fs::path {
    const char* workdir = git_repository_workdir(repo);
    return workdir ? fs::path(Util::fromUtf8(workdir)).lexically_normal() : fs::path();
}

/// dir relative to the working directory, with "/" ("" for the working directory itself)
auto prefixOf(git_repository* repo, const fs::path& dir) -> std::string {
    std::error_code error;
    const auto top = fs::weakly_canonical(workdirOf(repo), error);
    const auto canonical = fs::weakly_canonical(dir, error);
    auto relative = canonical.lexically_relative(top).generic_u8string();
    std::string prefix(relative.begin(), relative.end());
    if (prefix == "." || prefix.empty()) {
        return {};
    }
    return prefix;
}

/// Whether a repository path ("a/b.xopp") lies below prefix
auto isBelow(const std::string& prefix, const char* path) -> bool {
    if (prefix.empty()) {
        return true;
    }
    return std::strncmp(path, prefix.c_str(), prefix.size()) == 0 && path[prefix.size()] == '/';
}

/// The backups Xournal++ writes while saving are never committed
auto isRecoveryCopy(const char* path) -> bool {
    return AnnotationFiles::isRecoveryCopy(Util::fromUtf8(path).filename());
}

auto signatureFor(git_repository* repo) -> Signature {
    git_signature* sig = nullptr;
    if (git_signature_default(&sig, repo) == 0) {
        return Signature(sig);
    }
    // No user.name/user.email configured (git would refuse): sign with the account and the computer
    const char* name = g_get_real_name();
    if (!name || !*name || std::strcmp(name, "Unknown") == 0) {
        name = g_get_user_name();
    }
    const std::string email = std::string(g_get_user_name()) + "@" + g_get_host_name();
    git_signature_now(&sig, name, email.c_str());
    return Signature(sig);
}

/// Git LFS stores files as pointers that only the git-lfs program fills in; libgit2 cannot, so such repositories
/// must not be synced with it (pulled PDFs would be replaced by pointer files)
auto usesLfs(git_repository* repo, const git_tree* extraTree = nullptr) -> bool {
    auto mentionsLfs = [repo](const git_oid* blobId) {
        git_blob* blob = nullptr;
        if (git_blob_lookup(&blob, repo, blobId) != 0) {
            return false;
        }
        Blob guard(blob);
        const auto* content = static_cast<const char*>(git_blob_rawcontent(blob));
        const std::string text(content, static_cast<size_t>(git_blob_rawsize(blob)));
        return text.find("filter=lfs") != std::string::npos;
    };
    git_index* rawIndex = nullptr;
    if (git_repository_index(&rawIndex, repo) == 0) {
        Index index(rawIndex);
        const size_t count = git_index_entrycount(rawIndex);
        for (size_t i = 0; i < count; i++) {
            const git_index_entry* entry = git_index_get_byindex(rawIndex, i);
            const char* slash = std::strrchr(entry->path, '/');
            if (std::strcmp(slash ? slash + 1 : entry->path, ".gitattributes") == 0 && mentionsLfs(&entry->id)) {
                return true;
            }
        }
    }
    if (extraTree) {
        if (const git_tree_entry* entry = git_tree_entry_byname(extraTree, ".gitattributes")) {
            return mentionsLfs(git_tree_entry_id(entry));
        }
    }
    return false;
}

// --- Credentials ---

#ifdef _WIN32
auto widen(const std::string& value) -> std::wstring {
    gunichar2* converted = g_utf8_to_utf16(value.c_str(), -1, nullptr, nullptr, nullptr);
    std::wstring result(converted ? reinterpret_cast<wchar_t*>(converted) : L"");
    g_free(converted);
    return result;
}

auto narrow(const wchar_t* value, glong length = -1) -> std::string {
    gchar* converted = g_utf16_to_utf8(reinterpret_cast<const gunichar2*>(value), length, nullptr, nullptr, nullptr);
    std::string result(converted ? converted : "");
    g_free(converted);
    return result;
}
#endif

struct UrlParts {
    std::string scheme;
    std::string user;
    std::string host;
};

auto parseUrl(const std::string& url) -> UrlParts {
    UrlParts parts;
    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) {
        return parts;
    }
    parts.scheme = url.substr(0, schemeEnd);
    auto rest = url.substr(schemeEnd + 3);
    rest = rest.substr(0, rest.find('/'));
    if (const auto at = rest.rfind('@'); at != std::string::npos) {
        parts.user = rest.substr(0, rest.find(':') < at ? rest.find(':') : at);
        rest = rest.substr(at + 1);
    }
    parts.host = rest;
    return parts;
}

/**
 * The credentials Git for Windows keeps in the Windows credential manager (Git Credential Manager and the wincred
 * helper use the target "git:https://host"), so a sync works without a new sign-in where git was used before. The
 * ones entered here are stored the same way.
 */
auto readStoredCredentials(const std::string& url) -> std::optional<Credentials> {
#ifdef _WIN32
    const auto parts = parseUrl(url);
    if (parts.host.empty()) {
        return std::nullopt;
    }
    std::vector<std::string> targets;
    if (!parts.user.empty()) {
        targets.push_back("git:" + parts.scheme + "://" + parts.user + "@" + parts.host);
    }
    targets.push_back("git:" + parts.scheme + "://" + parts.host);
    for (const auto& target: targets) {
        PCREDENTIALW cred = nullptr;
        if (!CredReadW(widen(target).c_str(), CRED_TYPE_GENERIC, 0, &cred)) {
            continue;
        }
        Credentials result;
        result.username = cred->UserName ? narrow(cred->UserName) : parts.user;
        const auto* blob = cred->CredentialBlob;
        const DWORD size = cred->CredentialBlobSize;
        // Git Credential Manager stores UTF-8, git's wincred helper UTF-16
        bool utf16 = size >= 2 && size % 2 == 0;
        for (DWORD i = 1; utf16 && i < size; i += 2) {
            utf16 = blob[i] == 0;
        }
        result.password = utf16 ? narrow(reinterpret_cast<const wchar_t*>(blob), static_cast<glong>(size / 2)) :
                                  std::string(reinterpret_cast<const char*>(blob), size);
        result.remember = false;  // already stored
        CredFree(cred);
        return result;
    }
#else
    (void)url;
#endif
    return std::nullopt;
}

void storeCredentials(const std::string& url, const Credentials& credentials) {
#ifdef _WIN32
    const auto parts = parseUrl(url);
    if (parts.host.empty()) {
        return;
    }
    const auto target = widen("git:" + parts.scheme + "://" + parts.host);
    const auto user = widen(credentials.username);
    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<wchar_t*>(target.c_str());
    cred.UserName = const_cast<wchar_t*>(user.c_str());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(credentials.password.data()));
    cred.CredentialBlobSize = static_cast<DWORD>(credentials.password.size());
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    CredWriteW(&cred, 0);
#else
    (void)url;
    (void)credentials;
#endif
}

struct AuthContext {
    const CredentialPrompt* prompt = nullptr;  ///< nullptr: only stored credentials and keys, never ask
    std::string url;
    std::optional<Credentials> used;
    bool fromPrompt = false;
    bool triedStored = false;
    bool triedDefault = false;
    bool triedAgent = false;
    size_t keyIndex = 0;
    int prompts = 0;
    bool authFailed = false;
};

auto sshKeyFiles() -> std::vector<fs::path> {
    std::vector<fs::path> keys;
    const auto home = Util::fromUtf8(g_get_home_dir());
    for (const char* name: {"id_ed25519", "id_ecdsa", "id_rsa"}) {
        std::error_code error;
        if (fs::is_regular_file(home / ".ssh" / name, error)) {
            keys.push_back(home / ".ssh" / name);
        }
    }
    return keys;
}

auto acquireCredentials(git_credential** out, const char* url, const char* usernameFromUrl, unsigned int allowed,
                        void* payload) -> int {
    auto* ctx = static_cast<AuthContext*>(payload);
    ctx->url = url ? url : "";
    if (allowed & GIT_CREDENTIAL_SSH_KEY) {
        const char* user = usernameFromUrl && *usernameFromUrl ? usernameFromUrl : "git";
        if (!ctx->triedAgent) {
            ctx->triedAgent = true;
            return git_credential_ssh_key_from_agent(out, user);
        }
        const auto keys = sshKeyFiles();
        if (ctx->keyIndex < keys.size()) {
            const auto key = keys[ctx->keyIndex++];
            auto pub = key;
            pub += ".pub";
            std::error_code error;
            const auto pubText = Util::toUtf8(pub);
            return git_credential_ssh_key_new(out, user, fs::exists(pub, error) ? pubText.c_str() : nullptr,
                                              Util::toUtf8(key).c_str(), nullptr);
        }
        ctx->authFailed = true;
        return GIT_EAUTH;
    }
    if (allowed & GIT_CREDENTIAL_USERNAME) {
        return git_credential_username_new(out, usernameFromUrl && *usernameFromUrl ? usernameFromUrl : "git");
    }
    if (allowed & GIT_CREDENTIAL_USERPASS_PLAINTEXT) {
        if (!ctx->triedStored) {
            ctx->triedStored = true;
            if (auto stored = readStoredCredentials(ctx->url)) {
                ctx->used = std::move(stored);
                ctx->fromPrompt = false;
                return git_credential_userpass_plaintext_new(out, ctx->used->username.c_str(),
                                                             ctx->used->password.c_str());
            }
        }
        if (ctx->prompt && *ctx->prompt && ctx->prompts < 3) {
            ctx->prompts++;
            auto entered = (*ctx->prompt)(ctx->url, ctx->used);
            if (!entered) {
                ctx->authFailed = true;
                return GIT_EUSER;  // cancelled
            }
            ctx->used = std::move(entered);
            ctx->fromPrompt = true;
            return git_credential_userpass_plaintext_new(out, ctx->used->username.c_str(), ctx->used->password.c_str());
        }
        ctx->authFailed = true;
        return GIT_EAUTH;
    }
    if ((allowed & GIT_CREDENTIAL_DEFAULT) && !ctx->triedDefault) {
        ctx->triedDefault = true;  // Windows sign-in for servers that accept it (NTLM, Kerberos)
        return git_credential_default_new(out);
    }
    ctx->authFailed = true;
    return GIT_PASSTHROUGH;
}

/// After an operation: remember credentials the user typed once they worked
void finishAuth(const AuthContext& ctx, bool succeeded) {
    if (succeeded && ctx.fromPrompt && ctx.used && ctx.used->remember) {
        storeCredentials(ctx.url, *ctx.used);
    }
}

auto networkResult(int error, const AuthContext& ctx) -> Result {
    if (error == 0) {
        return {Outcome::Ok, {}};
    }
    if (error == GIT_EAUTH || error == GIT_EUSER || ctx.authFailed) {
        return {Outcome::AuthFailed, lastError()};
    }
    return failed();
}

void setCallbacks(git_remote_callbacks& callbacks, AuthContext& ctx) {
    callbacks.credentials = acquireCredentials;
    callbacks.payload = &ctx;
}

auto headBranch(git_repository* repo) -> Ref {
    git_reference* head = nullptr;
    if (git_repository_head(&head, repo) != 0) {
        return nullptr;
    }
    Ref ref(head);
    return git_reference_is_branch(head) ? std::move(ref) : nullptr;
}

auto upstreamRemote(git_repository* repo, git_reference* branch) -> std::optional<std::string> {
    git_buf buf = GIT_BUF_INIT;
    if (git_branch_upstream_remote(&buf, repo, git_reference_name(branch)) != 0) {
        return std::nullopt;
    }
    std::string name(buf.ptr, buf.size);
    git_buf_dispose(&buf);
    return name;
}

auto fetchRemote(git_repository* repo, const std::string& remoteName, AuthContext& ctx) -> int {
    git_remote* raw = nullptr;
    if (int error = git_remote_lookup(&raw, repo, remoteName.c_str()); error != 0) {
        return error;
    }
    Remote remote(raw);
    git_fetch_options options = GIT_FETCH_OPTIONS_INIT;
    setCallbacks(options.callbacks, ctx);
    return git_remote_fetch(raw, nullptr, &options, nullptr);
}

}  // namespace

auto ProjectGit::status(const fs::path& dir) -> RepoStatus {
    RepoStatus result;
    auto [repo, error] = openRepo(dir);
    if (error != 0) {
        if (error != GIT_ENOTFOUND) {
            result.error = lastError();
        }
        return result;
    }
    if (git_repository_is_bare(repo.get())) {
        return result;
    }
    result.isRepo = true;
    std::error_code fsError;
    result.toplevel = fs::weakly_canonical(workdirOf(repo.get()), fsError);
    const auto prefix = prefixOf(repo.get(), dir);

    git_status_options options = GIT_STATUS_OPTIONS_INIT;
    options.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    options.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
    StrArray pathspec(prefix.empty() ? std::vector<std::string>{} : std::vector<std::string>{prefix + "/"});
    if (!prefix.empty()) {
        options.pathspec = pathspec.array;
    }
    git_status_list* rawList = nullptr;
    if (git_status_list_new(&rawList, repo.get(), &options) != 0) {
        result.isRepo = false;
        result.error = lastError();
        return result;
    }
    StatusList list(rawList);
    Status& status = result.status;
    const size_t count = git_status_list_entrycount(rawList);
    for (size_t i = 0; i < count; i++) {
        const git_status_entry* entry = git_status_byindex(rawList, i);
        if (entry->status == GIT_STATUS_CURRENT || (entry->status & GIT_STATUS_IGNORED)) {
            continue;
        }
        const git_diff_delta* delta = entry->index_to_workdir ? entry->index_to_workdir : entry->head_to_index;
        if (!delta) {
            continue;
        }
        const char* path = delta->new_file.path ? delta->new_file.path : delta->old_file.path;
        if (!isBelow(prefix, path) || isRecoveryCopy(path)) {
            continue;
        }
        status.changedFiles++;
        const auto absolute = result.toplevel / Util::fromUtf8(path);
        if (entry->status & (GIT_STATUS_INDEX_DELETED | GIT_STATUS_WT_DELETED)) {
            status.deleted.push_back(absolute);
            continue;
        }
        const bool added = entry->status & (GIT_STATUS_WT_NEW | GIT_STATUS_INDEX_NEW);
        status.files[pathKey(absolute)] = added ? FileState::Added : FileState::Modified;
    }

    if (auto head = headBranch(repo.get())) {
        git_reference* upstream = nullptr;
        if (git_branch_upstream(&upstream, head.get()) == 0) {
            Ref upstreamGuard(upstream);
            status.hasUpstream = true;
            const git_oid* local = git_reference_target(head.get());
            const git_oid* remote = git_reference_target(upstream);
            if (local && remote) {
                git_graph_ahead_behind(&status.ahead, &status.behind, repo.get(), local, remote);
            }
        }
    }
    return result;
}

auto ProjectGit::fetch(const fs::path& toplevel) -> Result {
    auto [repo, error] = openRepo(toplevel);
    if (error != 0) {
        return failed();
    }
    auto head = headBranch(repo.get());
    if (!head) {
        return {Outcome::Ok, {}};
    }
    const auto remote = upstreamRemote(repo.get(), head.get());
    if (!remote) {
        return {Outcome::Ok, {}};
    }
    AuthContext ctx;  // no prompt: a background fetch only uses stored credentials
    return networkResult(fetchRemote(repo.get(), *remote, ctx), ctx);
}

auto ProjectGit::commitAll(const fs::path& dir, const std::string& message) -> Result {
    auto [repo, error] = openRepo(dir);
    if (error != 0) {
        return failed();
    }
    if (usesLfs(repo.get())) {
        return {Outcome::NotSupported, "Git LFS"};
    }
    const auto prefix = prefixOf(repo.get(), dir);
    git_index* rawIndex = nullptr;
    if (git_repository_index(&rawIndex, repo.get()) != 0) {
        return failed();
    }
    Index index(rawIndex);

    // git add -A, limited to the project folder and without Xournal++'s recovery copies
    StrArray pathspec(prefix.empty() ? std::vector<std::string>{} : std::vector<std::string>{prefix + "/"});
    git_index_matched_path_cb skip = [](const char* path, const char*, void* payload) -> int {
        const auto& folder = *static_cast<const std::string*>(payload);
        return isBelow(folder, path) && !isRecoveryCopy(path) ? 0 : 1;
    };
    auto* payload = const_cast<std::string*>(&prefix);
    const git_strarray* spec = prefix.empty() ? nullptr : &pathspec.array;
    if (git_index_add_all(rawIndex, spec, GIT_INDEX_ADD_DEFAULT, skip, payload) != 0 ||
        git_index_update_all(rawIndex, spec, skip, payload) != 0 || git_index_write(rawIndex) != 0) {
        return failed();
    }

    // The commit takes the project folder from the index and everything else from HEAD, like `git commit -- .`
    Commit parent;
    Tree parentTree;
    if (auto head = headBranch(repo.get())) {
        git_commit* rawParent = nullptr;
        if (git_commit_lookup(&rawParent, repo.get(), git_reference_target(head.get())) == 0) {
            parent.reset(rawParent);
            git_tree* rawTree = nullptr;
            if (git_commit_tree(&rawTree, rawParent) == 0) {
                parentTree.reset(rawTree);
            }
        }
    }
    git_oid treeId;
    if (prefix.empty() || !parentTree) {
        if (git_index_write_tree(&treeId, rawIndex) != 0) {
            return failed();
        }
    } else {
        git_index* rawCombined = nullptr;
        if (git_index_new(&rawCombined) != 0) {
            return failed();
        }
        Index combined(rawCombined);
        if (git_index_read_tree(rawCombined, parentTree.get()) != 0) {
            return failed();
        }
        std::vector<std::string> outdated;
        for (size_t i = 0; i < git_index_entrycount(rawCombined); i++) {
            const git_index_entry* entry = git_index_get_byindex(rawCombined, i);
            if (isBelow(prefix, entry->path)) {
                outdated.emplace_back(entry->path);
            }
        }
        for (const auto& path: outdated) {
            git_index_remove(rawCombined, path.c_str(), 0);
        }
        for (size_t i = 0; i < git_index_entrycount(rawIndex); i++) {
            const git_index_entry* entry = git_index_get_byindex(rawIndex, i);
            if (isBelow(prefix, entry->path) && git_index_add(rawCombined, entry) != 0) {
                return failed();
            }
        }
        if (git_index_write_tree_to(&treeId, rawCombined, repo.get()) != 0) {
            return failed();
        }
    }
    if (parentTree && git_oid_equal(&treeId, git_tree_id(parentTree.get()))) {
        return {Outcome::Ok, {}};  // nothing to commit
    }
    git_tree* rawTree = nullptr;
    if (git_tree_lookup(&rawTree, repo.get(), &treeId) != 0) {
        return failed();
    }
    Tree tree(rawTree);
    auto sig = signatureFor(repo.get());
    if (!sig) {
        return failed();
    }
    const git_commit* parents[] = {parent.get()};
    git_oid commitId;
    if (git_commit_create(&commitId, repo.get(), "HEAD", sig.get(), sig.get(), nullptr, message.c_str(), rawTree,
                          parent ? 1 : 0, parents) != 0) {
        return failed();
    }
    return {Outcome::Ok, {}};
}

auto ProjectGit::pullRebase(const fs::path& toplevel, const CredentialPrompt& prompt) -> Result {
    auto [repo, error] = openRepo(toplevel);
    if (error != 0) {
        return failed();
    }
    auto head = headBranch(repo.get());
    if (!head) {
        return failed("No branch is checked out.");
    }
    const auto remote = upstreamRemote(repo.get(), head.get());
    if (!remote) {
        return {Outcome::Ok, {}};
    }
    if (usesLfs(repo.get())) {
        return {Outcome::NotSupported, "Git LFS"};
    }
    AuthContext ctx;
    ctx.prompt = &prompt;
    const int fetchError = fetchRemote(repo.get(), *remote, ctx);
    finishAuth(ctx, fetchError == 0);
    if (fetchError != 0) {
        return networkResult(fetchError, ctx);
    }

    git_reference* rawUpstream = nullptr;
    if (git_branch_upstream(&rawUpstream, head.get()) != 0) {
        return {Outcome::Ok, {}};  // the remote branch does not exist (yet)
    }
    Ref upstream(rawUpstream);
    const git_oid local = *git_reference_target(head.get());
    const git_oid incoming = *git_reference_target(upstream.get());
    size_t ahead = 0, behind = 0;
    if (git_graph_ahead_behind(&ahead, &behind, repo.get(), &local, &incoming) != 0) {
        return failed();
    }
    if (behind == 0) {
        return {Outcome::Ok, {}};
    }
    git_commit* rawIncoming = nullptr;
    if (git_commit_lookup(&rawIncoming, repo.get(), &incoming) != 0) {
        return failed();
    }
    Commit incomingCommit(rawIncoming);
    git_tree* rawIncomingTree = nullptr;
    if (git_commit_tree(&rawIncomingTree, rawIncoming) == 0) {
        Tree incomingTree(rawIncomingTree);
        if (usesLfs(repo.get(), rawIncomingTree)) {
            return {Outcome::NotSupported, "Git LFS"};
        }
    }

    if (ahead == 0) {
        // Fast-forward: update the files, then the branch; SAFE keeps (and refuses to overwrite) local changes
        git_checkout_options checkout = GIT_CHECKOUT_OPTIONS_INIT;
        checkout.checkout_strategy = GIT_CHECKOUT_SAFE;
        if (git_checkout_tree(repo.get(), reinterpret_cast<const git_object*>(rawIncoming), &checkout) != 0) {
            return {Outcome::Conflict, lastError()};
        }
        git_reference* moved = nullptr;
        if (git_reference_set_target(&moved, head.get(), &incoming, "pull: fast-forward") != 0) {
            return failed();
        }
        git_reference_free(moved);
        return {Outcome::Ok, {}};
    }

    // Rebase the local commits onto the incoming ones
    git_annotated_commit* rawBranch = nullptr;
    git_annotated_commit* rawOnto = nullptr;
    if (git_annotated_commit_from_ref(&rawBranch, repo.get(), head.get()) != 0) {
        return failed();
    }
    Annotated branch(rawBranch);
    if (git_annotated_commit_lookup(&rawOnto, repo.get(), &incoming) != 0) {
        return failed();
    }
    Annotated onto(rawOnto);
    git_rebase_options options = GIT_REBASE_OPTIONS_INIT;
    options.checkout_options.checkout_strategy = GIT_CHECKOUT_SAFE;
    git_rebase* rawRebase = nullptr;
    if (git_rebase_init(&rawRebase, repo.get(), rawBranch, rawOnto, nullptr, &options) != 0) {
        return {Outcome::Conflict, lastError()};
    }
    Rebase rebase(rawRebase);
    auto sig = signatureFor(repo.get());
    auto abort = [&](Outcome outcome) -> Result {
        Result result{outcome, lastError()};
        git_rebase_abort(rawRebase);
        return result;
    };
    git_rebase_operation* operation = nullptr;
    int step = 0;
    while ((step = git_rebase_next(&operation, rawRebase)) == 0) {
        git_index* rawIndex = nullptr;
        if (git_repository_index(&rawIndex, repo.get()) != 0) {
            return abort(Outcome::Failed);
        }
        Index index(rawIndex);
        if (git_index_has_conflicts(rawIndex)) {
            return abort(Outcome::Conflict);
        }
        git_oid id;
        const int committed = git_rebase_commit(&id, rawRebase, nullptr, sig.get(), nullptr, nullptr);
        if (committed != 0 && committed != GIT_EAPPLIED) {  // EAPPLIED: the change is already upstream
            return abort(Outcome::Failed);
        }
    }
    if (step != GIT_ITEROVER) {
        return abort(step == GIT_ECONFLICT || step == GIT_EMERGECONFLICT ? Outcome::Conflict : Outcome::Failed);
    }
    if (git_rebase_finish(rawRebase, sig.get()) != 0) {
        return abort(Outcome::Failed);
    }
    return {Outcome::Ok, {}};
}

auto ProjectGit::push(const fs::path& toplevel, const CredentialPrompt& prompt) -> Result {
    auto [repo, error] = openRepo(toplevel);
    if (error != 0) {
        return failed();
    }
    auto head = headBranch(repo.get());
    if (!head) {
        return {Outcome::Ok, {}};  // nothing committed yet
    }
    const char* branchName = nullptr;
    git_branch_name(&branchName, head.get());
    std::string remoteName;
    std::string destination;
    bool setUpstream = false;
    if (auto upstream = upstreamRemote(repo.get(), head.get())) {
        remoteName = *upstream;
        git_buf merge = GIT_BUF_INIT;
        if (git_branch_upstream_merge(&merge, repo.get(), git_reference_name(head.get())) != 0) {
            return failed();
        }
        destination.assign(merge.ptr, merge.size);
        git_buf_dispose(&merge);
    } else {
        // No upstream yet: publish to "origin", else the first remote, like `git push -u <remote> HEAD`
        git_strarray remotes{};
        if (git_remote_list(&remotes, repo.get()) != 0) {
            return failed();
        }
        for (size_t i = 0; i < remotes.count; i++) {
            if (remoteName.empty() || std::strcmp(remotes.strings[i], "origin") == 0) {
                remoteName = remotes.strings[i];
            }
        }
        git_strarray_dispose(&remotes);
        if (remoteName.empty()) {
            return {Outcome::Ok, {}};  // a local repository
        }
        destination = std::string("refs/heads/") + branchName;
        setUpstream = true;
    }
    git_remote* rawRemote = nullptr;
    if (git_remote_lookup(&rawRemote, repo.get(), remoteName.c_str()) != 0) {
        return failed();
    }
    Remote remote(rawRemote);

    AuthContext ctx;
    ctx.prompt = &prompt;
    std::string rejection;
    git_push_options options = GIT_PUSH_OPTIONS_INIT;
    setCallbacks(options.callbacks, ctx);
    struct PushPayload {
        AuthContext* ctx;
        std::string* rejection;
    } payload{&ctx, &rejection};
    // The credential callback gets the AuthContext; the reference callback needs the rejection too
    options.callbacks.payload = &payload;
    options.callbacks.credentials = [](git_credential** out, const char* url, const char* user, unsigned int allowed,
                                       void* p) -> int {
        return acquireCredentials(out, url, user, allowed, static_cast<PushPayload*>(p)->ctx);
    };
    options.callbacks.push_update_reference = [](const char* refname, const char* status, void* p) -> int {
        if (status) {  // rejected, e.g. "fetch first" when the remote has new commits
            *static_cast<PushPayload*>(p)->rejection = std::string(refname) + ": " + status;
        }
        return 0;
    };
    StrArray refspecs({std::string(git_reference_name(head.get())) + ":" + destination});
    const int pushError = git_remote_push(rawRemote, &refspecs.array, &options);
    finishAuth(ctx, pushError == 0);
    if (pushError != 0) {
        return networkResult(pushError, ctx);
    }
    if (!rejection.empty()) {
        return failed(rejection);
    }
    if (setUpstream) {
        git_branch_set_upstream(head.get(), (remoteName + "/" + branchName).c_str());
    }
    return {Outcome::Ok, {}};
}

auto ProjectGit::restore(const fs::path& toplevel, const std::vector<fs::path>& paths) -> Result {
    auto [repo, error] = openRepo(toplevel);
    if (error != 0) {
        return failed();
    }
    // Pathspecs, so a folder covers its files; glob characters in names are escaped to stay literal
    std::vector<std::string> relative;
    for (const auto& path: paths) {
        std::string escaped;
        for (char c: prefixOf(repo.get(), path)) {
            if (c == '*' || c == '?' || c == '[' || c == '\\') {
                escaped += '\\';
            }
            escaped += c;
        }
        relative.push_back(escaped);
    }
    StrArray pathspec(relative);
    git_object* rawHead = nullptr;
    if (git_revparse_single(&rawHead, repo.get(), "HEAD^{commit}") != 0) {
        return failed();
    }
    Object head(rawHead);
    // --staged: the index entries back to HEAD; --worktree: the files back to HEAD
    if (git_reset_default(repo.get(), rawHead, &pathspec.array) != 0) {
        return failed();
    }
    git_checkout_options checkout = GIT_CHECKOUT_OPTIONS_INIT;
    checkout.checkout_strategy = GIT_CHECKOUT_FORCE;
    checkout.paths = pathspec.array;
    if (git_checkout_head(repo.get(), &checkout) != 0) {
        return failed();
    }
    return {Outcome::Ok, {}};
}

auto ProjectGit::pathKey(const fs::path& path) -> std::string {
    auto normalized = path.lexically_normal();
    normalized.make_preferred();
    auto key = Util::toUtf8(normalized);
    while (key.size() > 1 && (key.back() == '/' || key.back() == '\\')) {
        key.pop_back();
    }
#ifdef _WIN32
    gchar* folded = g_utf8_casefold(key.c_str(), -1);
    key = folded;
    g_free(folded);
#endif
    return key;
}

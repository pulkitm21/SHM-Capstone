import { useEffect, useMemo, useState } from "react";
import useAuth from "../../Auth/useAuth";
import {
  createUser,
  deleteUser,
  getUsers,
  resetUserPassword,
  updateUserRole,
  type AuthRole,
  type AuthUser,
} from "../../services/api";
import "./Users.css";

type CreateFormState = {
  username: string;
  password: string;
  role: AuthRole;
};

type PasswordDrafts = Record<number, string>;
type SavingUserMap = Record<number, boolean>;

const PASSWORD_RULE_MESSAGE =
  "Password must be at least 8 characters and include uppercase, lowercase, number, and special character.";

const INITIAL_CREATE_FORM: CreateFormState = {
  username: "",
  password: "",
  role: "viewer",
};

function isStrongPassword(password: string) {
  const trimmed = password.trim();

  return (
    trimmed.length >= 8 &&
    /[a-z]/.test(trimmed) &&
    /[A-Z]/.test(trimmed) &&
    /\d/.test(trimmed) &&
    /[^A-Za-z0-9]/.test(trimmed)
  );
}

function formatDateTime(value?: string | null) {
  if (!value) return "Never";

  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return value;

  return date.toLocaleString();
}

function Users() {
  const { user: currentUser } = useAuth();

  const [users, setUsers] = useState<AuthUser[]>([]);
  const [isLoading, setIsLoading] = useState(true);
  const [pageError, setPageError] = useState("");
  const [pageMessage, setPageMessage] = useState("");

  const [createForm, setCreateForm] = useState<CreateFormState>(INITIAL_CREATE_FORM);
  const [isCreatingUser, setIsCreatingUser] = useState(false);
  const [createError, setCreateError] = useState("");

  const [passwordDrafts, setPasswordDrafts] = useState<PasswordDrafts>({});
  const [passwordErrors, setPasswordErrors] = useState<Record<number, string>>({});
  const [roleSavingMap, setRoleSavingMap] = useState<SavingUserMap>({});
  const [passwordSavingMap, setPasswordSavingMap] = useState<SavingUserMap>({});
  const [deleteSavingMap, setDeleteSavingMap] = useState<SavingUserMap>({});

  /*
    Loads the current visible users from the backend. The built-in developer
    account is expected to be hidden by the backend and should not appear here.
  */
  async function loadUsers() {
    try {
      setPageError("");
      const response = await getUsers();
      setUsers(response.users);
    } catch (error) {
      const message =
        error instanceof Error ? error.message : "Failed to load users.";
      setPageError(message);
    } finally {
      setIsLoading(false);
    }
  }

  useEffect(() => {
    void loadUsers();
  }, []);

  /*
    A normalized lowercase username list helps prevent duplicate creates
    in the UI before the backend is called.
  */
  const normalizedUsernames = useMemo(
    () => new Set(users.map((item) => item.username.trim().toLowerCase())),
    [users]
  );

  function updateCreateField<K extends keyof CreateFormState>(
    key: K,
    value: CreateFormState[K]
  ) {
    setCreateForm((prev) => ({ ...prev, [key]: value }));
  }

  async function handleCreateUser(event: React.FormEvent) {
    event.preventDefault();
    setCreateError("");
    setPageMessage("");

    const username = createForm.username.trim();
    const password = createForm.password.trim();

    if (!username) {
      setCreateError("Username is required.");
      return;
    }

    if (normalizedUsernames.has(username.toLowerCase())) {
      setCreateError("A user with that username already exists.");
      return;
    }

    if (!isStrongPassword(password)) {
      setCreateError(PASSWORD_RULE_MESSAGE);
      return;
    }

    try {
      setIsCreatingUser(true);

      const response = await createUser({
        username,
        password,
        role: createForm.role,
      });

      /*
        New users are appended optimistically so the page feels responsive
        even before a full refresh from the backend.
      */
      setUsers((prev) =>
        [...prev, response.user].sort((a, b) =>
          a.username.localeCompare(b.username, undefined, { sensitivity: "base" })
        )
      );

      setCreateForm(INITIAL_CREATE_FORM);
      setPageMessage(`User "${response.user.username}" created successfully.`);
    } catch (error) {
      const message =
        error instanceof Error ? error.message : "Failed to create user.";
      setCreateError(message);
    } finally {
      setIsCreatingUser(false);
    }
  }

  async function handleRoleChange(nextRole: AuthRole, targetUser: AuthUser) {
    if (!targetUser.id) return;

    setPageError("");
    setPageMessage("");
    setRoleSavingMap((prev) => ({ ...prev, [targetUser.id as number]: true }));

    try {
      const response = await updateUserRole(targetUser.id, { role: nextRole });

      setUsers((prev) =>
        prev.map((item) => (item.id === targetUser.id ? response.user : item))
      );

      setPageMessage(`Role updated for "${response.user.username}".`);
    } catch (error) {
      const message =
        error instanceof Error ? error.message : "Failed to update role.";
      setPageError(message);
    } finally {
      setRoleSavingMap((prev) => ({ ...prev, [targetUser.id as number]: false }));
    }
  }

  function handlePasswordDraftChange(userId: number, value: string) {
    setPasswordDrafts((prev) => ({ ...prev, [userId]: value }));

    /*
      Clear the row-level password error when the admin edits the field again.
    */
    setPasswordErrors((prev) => {
      if (!prev[userId]) return prev;
      return { ...prev, [userId]: "" };
    });
  }

  async function handlePasswordReset(targetUser: AuthUser) {
    if (!targetUser.id) return;

    const nextPassword = (passwordDrafts[targetUser.id] || "").trim();

    if (!isStrongPassword(nextPassword)) {
      setPasswordErrors((prev) => ({
        ...prev,
        [targetUser.id as number]: PASSWORD_RULE_MESSAGE,
      }));
      return;
    }

    setPageError("");
    setPageMessage("");
    setPasswordSavingMap((prev) => ({ ...prev, [targetUser.id as number]: true }));

    try {
      await resetUserPassword(targetUser.id, { password: nextPassword });

      setPasswordDrafts((prev) => ({ ...prev, [targetUser.id as number]: "" }));
      setPasswordErrors((prev) => ({ ...prev, [targetUser.id as number]: "" }));
      setPageMessage(`Password updated for "${targetUser.username}".`);
    } catch (error) {
      const message =
        error instanceof Error ? error.message : "Failed to reset password.";

      setPasswordErrors((prev) => ({
        ...prev,
        [targetUser.id as number]: message,
      }));
    } finally {
      setPasswordSavingMap((prev) => ({ ...prev, [targetUser.id as number]: false }));
    }
  }

  async function handleDeleteUser(targetUser: AuthUser) {
    if (!targetUser.id) return;

    if (currentUser?.id === targetUser.id) {
      setPageError("You cannot delete the currently signed-in account.");
      return;
    }

    const confirmed = window.confirm(
      `Delete user "${targetUser.username}"? This action cannot be undone.`
    );

    if (!confirmed) return;

    setPageError("");
    setPageMessage("");
    setDeleteSavingMap((prev) => ({ ...prev, [targetUser.id as number]: true }));

    try {
      await deleteUser(targetUser.id);

      setUsers((prev) => prev.filter((item) => item.id !== targetUser.id));
      setPageMessage(`User "${targetUser.username}" deleted successfully.`);
    } catch (error) {
      const message =
        error instanceof Error ? error.message : "Failed to delete user.";
      setPageError(message);
    } finally {
      setDeleteSavingMap((prev) => ({ ...prev, [targetUser.id as number]: false }));
    }
  }

  return (
    <div className="users-page">
      <div className="users-hero">
        <div>
          <h1 className="users-title">Users</h1>
          <p className="users-subtitle">
            Manage dashboard access, assign roles, and reset user passwords.
          </p>
        </div>
      </div>

      <div className="users-grid">
        <section className="users-card">
          <div className="users-card-header">
            <h2>Create User</h2>
            <p>Add a new dashboard account and assign its role.</p>
          </div>

          <form className="users-create-form" onSubmit={handleCreateUser}>
            <label className="users-field">
              <span>Username</span>
              <input
                type="text"
                value={createForm.username}
                onChange={(event) => updateCreateField("username", event.target.value)}
                placeholder="Enter username"
                disabled={isCreatingUser}
              />
            </label>

            <label className="users-field">
              <span>Password</span>
              <input
                type="password"
                value={createForm.password}
                onChange={(event) => updateCreateField("password", event.target.value)}
                placeholder="Enter temporary password"
                disabled={isCreatingUser}
              />
            </label>

            <label className="users-field">
              <span>Role</span>
              <select
                value={createForm.role}
                onChange={(event) =>
                  updateCreateField("role", event.target.value as AuthRole)
                }
                disabled={isCreatingUser}
              >
                <option value="viewer">Viewer</option>
                <option value="admin">Admin</option>
              </select>
            </label>

            <p className="users-helper-text">{PASSWORD_RULE_MESSAGE}</p>

            {createError ? <div className="users-inline-error">{createError}</div> : null}

            <button
              type="submit"
              className="users-primary-button"
              disabled={isCreatingUser}
            >
              {isCreatingUser ? "Creating..." : "Create User"}
            </button>
          </form>
        </section>

        <section className="users-card users-card-wide">
          <div className="users-card-header">
            <h2>User Management</h2>
            <p>Change roles, reset passwords, or remove existing accounts.</p>
          </div>

          {pageError ? <div className="users-banner users-banner-error">{pageError}</div> : null}
          {pageMessage ? (
            <div className="users-banner users-banner-success">{pageMessage}</div>
          ) : null}

          {isLoading ? (
            <div className="users-empty-state">Loading users...</div>
          ) : users.length === 0 ? (
            <div className="users-empty-state">No managed users found.</div>
          ) : (
            <div className="users-table-wrap">
              <table className="users-table">
                <thead>
                  <tr>
                    <th>Username</th>
                    <th>Role</th>
                    <th>Last Login</th>
                    <th>Password Reset</th>
                    <th>Delete</th>
                  </tr>
                </thead>

                <tbody>
                  {users.map((item) => {
                    const userId = item.id;
                    const isCurrentUser = currentUser?.id === item.id;

                    return (
                      <tr key={item.id ?? item.username}>
                        <td>
                          <div className="users-username-cell">
                            <span className="users-username">{item.username}</span>
                            {isCurrentUser ? (
                              <span className="users-self-pill">Current account</span>
                            ) : null}
                          </div>
                        </td>

                        <td>
                          <select
                            className="users-role-select"
                            value={item.role}
                            disabled={!userId || !!roleSavingMap[userId]}
                            onChange={(event) =>
                              void handleRoleChange(
                                event.target.value as AuthRole,
                                item
                              )
                            }
                          >
                            <option value="viewer">Viewer</option>
                            <option value="admin">Admin</option>
                          </select>
                        </td>

                        <td>{formatDateTime(item.last_login_at)}</td>

                        <td>
                          <div className="users-password-cell">
                            <input
                              type="password"
                              value={(userId && passwordDrafts[userId]) || ""}
                              onChange={(event) =>
                                userId &&
                                handlePasswordDraftChange(userId, event.target.value)
                              }
                              placeholder="Enter new password"
                              disabled={!userId || !!passwordSavingMap[userId]}
                            />

                            <button
                              type="button"
                              className="users-secondary-button"
                              disabled={!userId || !!passwordSavingMap[userId]}
                              onClick={() => void handlePasswordReset(item)}
                            >
                              {userId && passwordSavingMap[userId]
                                ? "Saving..."
                                : "Reset"}
                            </button>

                            {userId && passwordErrors[userId] ? (
                              <div className="users-row-error">
                                {passwordErrors[userId]}
                              </div>
                            ) : null}
                          </div>
                        </td>

                        <td>
                          <button
                            type="button"
                            className="users-danger-button"
                            disabled={
                              !userId || isCurrentUser || !!deleteSavingMap[userId]
                            }
                            onClick={() => void handleDeleteUser(item)}
                            title={
                              isCurrentUser
                                ? "You cannot delete the current account."
                                : "Delete user"
                            }
                          >
                            {userId && deleteSavingMap[userId]
                              ? "Deleting..."
                              : isCurrentUser
                              ? "Blocked"
                              : "Delete"}
                          </button>
                        </td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </div>
          )}
        </section>
      </div>
    </div>
  );
}

export default Users;
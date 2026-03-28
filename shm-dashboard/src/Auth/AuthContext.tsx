import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useState,
  type ReactNode,
} from "react";
import {
  getCurrentUser,
  login as loginRequest,
  logout as logoutRequest,
  type AuthRole,
  type AuthUser,
} from "../services/api";

type LoginCredentials = {
  username: string;
  password: string;
};

type AuthContextValue = {
  user: AuthUser | null;
  isAuthenticated: boolean;
  isAdmin: boolean;
  isLoading: boolean;
  login: (credentials: LoginCredentials) => Promise<void>;
  logout: () => Promise<void>;
  refreshAuth: () => Promise<void>;
};

const AuthContext = createContext<AuthContextValue | undefined>(undefined);

type AuthProviderProps = {
  children: ReactNode;
};

export function AuthProvider({ children }: AuthProviderProps) {
  const [user, setUser] = useState<AuthUser | null>(null);
  const [isLoading, setIsLoading] = useState(true);

  /*
    Loads the current authenticated user from the backend session endpoint.
    The backend is the source of truth for both login state and role.
  */
  const refreshAuth = useCallback(async () => {
    try {
      const response = await getCurrentUser();

      if (!response.authenticated || !response.user) {
        setUser(null);
        return;
      }

      setUser(response.user);
    } catch (error) {
      /*
        If the backend says the user is not authenticated, or the request
        fails, the frontend falls back to a logged-out state.
      */
      console.warn("Failed to refresh auth state.", error);
      setUser(null);
    }
  }, []);

  /*
    Sends credentials to the backend login endpoint. A successful login
    should set the session cookie on the backend response, then the current
    user is refreshed from /api/auth/me so frontend state stays consistent.
  */
  const login = useCallback(
    async (credentials: LoginCredentials) => {
      const normalizedUsername = credentials.username.trim();
      const normalizedPassword = credentials.password.trim();

      if (!normalizedUsername || !normalizedPassword) {
        throw new Error("Please enter both username and password.");
      }

      await loginRequest({
        username: normalizedUsername,
        password: normalizedPassword,
      });

      await refreshAuth();
    },
    [refreshAuth]
  );

  /*
    Calls the backend logout endpoint and clears local auth state even if
    the backend request fails, so the UI does not remain logged in locally.
  */
  const logout = useCallback(async () => {
    try {
      await logoutRequest();
    } catch (error) {
      console.warn("Failed to log out cleanly from backend.", error);
    } finally {
      setUser(null);
    }
  }, []);

  /*
    The provider performs an auth bootstrap on mount so route guards
    can make decisions only after the initial auth state is known.
  */
  useEffect(() => {
    let cancelled = false;

    async function bootstrapAuth() {
      try {
        await refreshAuth();
      } finally {
        if (!cancelled) {
          setIsLoading(false);
        }
      }
    }

    void bootstrapAuth();

    return () => {
      cancelled = true;
    };
  }, [refreshAuth]);

  const value = useMemo<AuthContextValue>(
    () => ({
      user,
      isAuthenticated: user !== null,
      isAdmin: user?.role === "admin",
      isLoading,
      login,
      logout,
      refreshAuth,
    }),
    [user, isLoading, login, logout, refreshAuth]
  );

  return <AuthContext.Provider value={value}>{children}</AuthContext.Provider>;
}

/*
  This hook provides safe access to the shared auth context and ensures
  it is only used inside the AuthProvider tree.
*/
export function useAuthContext() {
  const context = useContext(AuthContext);

  if (!context) {
    throw new Error("useAuthContext must be used within an AuthProvider.");
  }

  return context;
}
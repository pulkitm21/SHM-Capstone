import './Tiles.css';

interface Tile {
  id: string;
  title: string;
  count: number;
  color: string;
}

interface TilesProps {
  tiles: Tile[];
}

const Tiles = ({ tiles }: TilesProps) => {
  return (
    <div className="tiles-container">
      {tiles.map((tile) => (
        <div key={tile.id} className="tile-card">
          <div className="tile-info">
            <h3 className="tile-title">{tile.title}</h3>
            <p className="tile-count">{tile.count}</p>
          </div>
        </div>
      ))}
    </div>
  );
};

export default Tiles;